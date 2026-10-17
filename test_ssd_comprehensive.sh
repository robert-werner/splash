#!/bin/bash
# Test SSD-backed KV cache extension using Splash serve

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8082
CACHE_DIR="/tmp/splash_ssd_cache"

echo "=== Testing SSD tier functionality ==="
echo "Model: $MODEL"
echo "Port: $PORT"
echo "Cache dir: $CACHE_DIR"
echo ""

# Create cache directory
mkdir -p "$CACHE_DIR"

# Start server with SSD tier enabled
echo "Starting Splash server with SSD tier..."
cd /Users/a1111/development/splash_of_mine
./splash serve \
  --model "$MODEL" \
  --port $PORT \
  --working-set 16G \
  --max-cache-disk 10G \
  --persistent-cache \
  --cache-dir "$CACHE_DIR" \
  --state-format bf16 \
  --vision-text-only \
  > /tmp/splash_ssd_serve.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start (model download and preparation can take time)
echo "Waiting for server to start (this may take several minutes for first-time model download)..."
MAX_WAIT=300
ELAPSED=0
READY=false

while [ $ELAPSED -lt $MAX_WAIT ]; do
    if curl -s http://127.0.0.1:$PORT/health > /dev/null 2>&1; then
        READY=true
        echo "Server ready after $ELAPSED seconds"
        break
    fi
    
    # Check if server process is still running
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: Server process died during startup"
        echo "Last 100 lines of log:"
        tail -100 /tmp/splash_ssd_serve.log
        exit 1
    fi
    
    sleep 10
    ELAPSED=$((ELAPSED + 10))
    echo "  ... still waiting ($ELAPSED/$MAX_WAIT seconds)"
done

if [ "$READY" = false ]; then
    echo "ERROR: Server did not become ready within $MAX_WAIT seconds"
    echo "Last 100 lines of log:"
    tail -100 /tmp/splash_ssd_serve.log
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

# Test 1: Health check
echo ""
echo "=== Test 1: Server Health Check ==="
curl -s http://127.0.0.1:$PORT/health | jq '.' || echo "Health check returned data"

# Test 2: Initial metrics (before any KV cache usage)
echo ""
echo "=== Test 2: Initial Metrics (before load) ==="
echo "SSD-related metrics:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "(kv_pages|ssd_|extended_kv)" || echo "No SSD metrics yet"

# Test 3: Small context (should stay in RAM)
echo ""
echo "=== Test 3: Small Context (10K tokens, should stay in RAM) ==="
SMALL_PROMPT=$(python3 -c "print('word ' * 5000)")  # ~5K tokens
RESPONSE=$(curl -s -w "\nHTTP_CODE:%{http_code}" http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$SMALL_PROMPT Summarize in one sentence.\"}],
    \"max_tokens\": 20,
    \"stream\": false
  }")

HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE:" | cut -d: -f2)
BODY=$(echo "$RESPONSE" | grep -v "HTTP_CODE:")

if [ "$HTTP_CODE" = "200" ]; then
    echo "Request successful"
    echo "$BODY" | jq '.usage' 2>/dev/null || echo "Response received"
else
    echo "Request failed with code $HTTP_CODE"
    echo "$BODY"
fi

sleep 2

# Metrics after small context
echo ""
echo "Metrics after small context:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "(kv_pages|ssd_|extended_kv)"

# Test 4: Medium context (may trigger SSD tier)
echo ""
echo "=== Test 4: Medium Context (50K tokens, may trigger SSD) ==="
# Generate a larger prompt
LARGE_PROMPT=$(python3 -c "print('The quick brown fox jumps over the lazy dog. ' * 3000)")  # ~30K tokens
RESPONSE=$(curl -s -w "\nHTTP_CODE:%{http_code}" http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$LARGE_PROMPT\nPlease summarize the above text in one sentence.\"}],
    \"max_tokens\": 30,
    \"stream\": false
  }")

HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE:" | cut -d: -f2)
BODY=$(echo "$RESPONSE" | grep -v "HTTP_CODE:")

if [ "$HTTP_CODE" = "200" ]; then
    echo "Request successful"
    echo "$BODY" | jq '.usage' 2>/dev/null || echo "Response received"
else
    echo "Request failed with code $HTTP_CODE"
    echo "$BODY"
fi

sleep 2

# Test 5: Final metrics
echo ""
echo "=== Test 5: Final Metrics (after all tests) ==="
echo "All SSD-related metrics:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "(kv_pages|ssd_|extended_kv)" || echo "No SSD activity detected"

echo ""
echo "KV cache statistics:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "^kv_" | head -20

echo ""
echo "Metal memory usage:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "metal_memory"

# Check cache directory
echo ""
echo "=== Cache Directory Contents ==="
ls -lh "$CACHE_DIR/" 2>/dev/null || echo "Cache directory empty or not accessible"
du -sh "$CACHE_DIR/" 2>/dev/null || echo "Could not measure cache size"

# Cleanup
echo ""
echo "=== Cleanup ==="
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Test Complete ==="
echo "Full logs: /tmp/splash_ssd_serve.log"
echo "Cache dir: $CACHE_DIR"
echo ""
echo "SUMMARY:"
echo "- Server started successfully: $([ "$READY" = true ] && echo 'YES' || echo 'NO')"
echo "- Small context completed: $([ "$HTTP_CODE" = "200" ] && echo 'YES' || echo 'FAILED')"
echo "- Check metrics above for SSD tier activity (kv_pages_cold, ssd_read_bytes, extended_kv_*)"
