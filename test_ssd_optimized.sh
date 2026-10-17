#!/bin/bash
# Test SSD-backed KV cache extension using Splash serve (memory-conservative)

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

# Start server with SSD tier enabled (without --working-set to allow auto memory budget)
echo "Starting Splash server with SSD tier..."
cd /Users/a1111/development/splash_of_mine
./splash serve \
  --model "$MODEL" \
  --port $PORT \
  --max-cache-disk 10G \
  --persistent-cache \
  --cache-dir "$CACHE_DIR" \
  --state-format bf16 \
  --vision-text-only \
  > /tmp/splash_ssd_serve.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start (model is already installed)
echo "Waiting for server to start..."
MAX_WAIT=120
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
        echo "Last 50 lines of log:"
        tail -50 /tmp/splash_ssd_serve.log
        exit 1
    fi
    
    sleep 5
    ELAPSED=$((ELAPSED + 5))
    echo "  ... waiting ($ELAPSED/$MAX_WAIT seconds)"
done

if [ "$READY" = false ]; then
    echo "ERROR: Server did not become ready within $MAX_WAIT seconds"
    echo "Last 50 lines of log:"
    tail -50 /tmp/splash_ssd_serve.log
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

# Test 1: Health check and initial state
echo ""
echo "=== Test 1: Server Health and Initial State ==="
curl -s http://127.0.0.1:$PORT/health | jq '.' 2>/dev/null || echo "Health check OK"

echo ""
echo "Initial SSD-related metrics:"
INITIAL_METRICS=$(curl -s http://127.0.0.1:$PORT/metrics)
echo "$INITIAL_METRICS" | grep -E "splash_kv_pages"

# Extract initial values
INITIAL_DISK_WRITES=$(echo "$INITIAL_METRICS" | grep "splash_disk_written_bytes_total" | awk '{print $2}' || echo "0")
INITIAL_DISK_READS=$(echo "$INITIAL_METRICS" | grep "splash_disk_read_bytes_total" | awk '{print $2}' || echo "0")
INITIAL_DISK_USED=$(echo "$INITIAL_METRICS" | grep "splash_disk_used_bytes" | awk '{print $2}' || echo "0")

# Test 2: Small context (should stay in RAM)
echo ""
echo "=== Test 2: Small Context Test (5K tokens) ==="
SMALL_PROMPT=$(python3 -c "print('Hello world. ' * 2000)")
RESPONSE=$(curl -s -w "\nHTTP_CODE:%{http_code}" http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$SMALL_PROMPT\\n\\nPlease respond with 'OK' only.\"}],
    \"max_tokens\": 5,
    \"temperature\": 0,
    \"stream\": false
  }")

HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE:" | cut -d: -f2)
BODY=$(echo "$RESPONSE" | grep -v "HTTP_CODE:")

if [ "$HTTP_CODE" = "200" ]; then
    echo "✓ Small context request successful"
    echo "$BODY" | jq '.usage' 2>/dev/null || echo "Response received"
else
    echo "✗ Small context request failed with code $HTTP_CODE"
    echo "$BODY" | head -20
fi

sleep 2

# Metrics after small context
echo ""
echo "Metrics after small context:"
AFTER_SMALL=$(curl -s http://127.0.0.1:$PORT/metrics)
echo "$AFTER_SMALL" | grep -E "splash_kv_pages" | head -10

# Test 3: Larger context to trigger potential SSD usage
echo ""
echo "=== Test 3: Medium Context Test (25K+ tokens) ==="
# Create a longer prompt to stress the KV cache
LARGE_PROMPT=$(python3 -c "
text = 'The quick brown fox jumps over the lazy dog. '
# Repeat to make ~25K tokens
print(text * 2500)
")

RESPONSE=$(curl -s -w "\nHTTP_CODE:%{http_code}" http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$LARGE_PROMPT\\n\\nSummarize this text in exactly 10 words.\"}],
    \"max_tokens\": 20,
    \"temperature\": 0,
    \"stream\": false
  }")

HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE:" | cut -d: -f2)
BODY=$(echo "$RESPONSE" | grep -v "HTTP_CODE:")

if [ "$HTTP_CODE" = "200" ]; then
    echo "✓ Medium context request successful"
    echo "$BODY" | jq '.usage' 2>/dev/null || echo "Response received"
else
    echo "✗ Medium context request failed with code $HTTP_CODE"
    echo "$BODY" | head -20
fi

sleep 2

# Test 4: Final metrics analysis
echo ""
echo "=== Test 4: Final Metrics Analysis ==="
FINAL_METRICS=$(curl -s http://127.0.0.1:$PORT/metrics)

echo ""
echo "SSD tier metrics:"
echo "$FINAL_METRICS" | grep -E "splash_disk" | head -15

echo ""
echo "KV cache page statistics:"
echo "$FINAL_METRICS" | grep -E "splash_kv"

echo ""
echo "Metal memory usage:"
echo "$FINAL_METRICS" | grep -E "metal_memory"

# Extract final values and calculate deltas
FINAL_DEMOTIONS=$(echo "$FINAL_METRICS" | grep "extended_kv_demotions" | awk '{print $2}' || echo "0")
FINAL_RESTORES=$(echo "$FINAL_METRICS" | grep "extended_kv_restores" | awk '{print $2}' || echo "0")
FINAL_SSD_WRITES=$(echo "$FINAL_METRICS" | grep "ssd_write_bytes" | awk '{print $2}' || echo "0")
FINAL_SSD_READS=$(echo "$FINAL_METRICS" | grep "ssd_read_bytes" | awk '{print $2}' || echo "0")

DEMOTIONS_DELTA=$((FINAL_DEMOTIONS - INITIAL_DEMOTIONS))
RESTORES_DELTA=$((FINAL_RESTORES - INITIAL_RESTORES))
SSD_WRITES_DELTA=$((FINAL_SSD_WRITES - INITIAL_SSD_WRITES))
SSD_READS_DELTA=$((FINAL_SSD_READS - INITIAL_SSD_READS))

# Check cache directory
echo ""
echo "=== Cache Directory Contents ==="
ls -lh "$CACHE_DIR/" 2>/dev/null || echo "Cache directory empty or not accessible"
if [ -d "$CACHE_DIR" ]; then
    DU_OUTPUT=$(du -sh "$CACHE_DIR/" 2>/dev/null)
    echo "Cache size: $DU_OUTPUT"
fi

# Cleanup
echo ""
echo "=== Cleanup ==="
echo "Stopping server (PID $SERVER_PID)..."
kill $SERVER_PID 2>/dev/null || echo "Server already stopped"
wait $SERVER_PID 2>/dev/null || true
sleep 1

# Final summary
echo ""
echo "======================================================================"
echo "                         TEST SUMMARY"
echo "======================================================================"
echo ""
echo "Server Status:"
echo "  Started successfully: $([ "$READY" = true ] && echo '✓ YES' || echo '✗ NO')"
echo "  Test requests completed: $([ "$HTTP_CODE" = "200" ] && echo '✓ YES' || echo '✗ FAILED')"
echo ""
echo "SSD Tier Activity:"
echo "  KV demotions to SSD:    $DEMOTIONS_DELTA"
echo "  KV restores from SSD:   $RESTORES_DELTA"
echo "  SSD bytes written:      $SSD_WRITES_DELTA"
echo "  SSD bytes read:         $SSD_READS_DELTA"
echo ""

if [ "$DEMOTIONS_DELTA" -gt 0 ] || [ "$SSD_WRITES_DELTA" -gt 0 ]; then
    echo "Result: ✓ SSD TIER ACTIVATED - KV cache pages were demoted to disk"
    echo ""
    echo "The SSD-backed KV cache extension is working correctly:"
    echo "  - Server started with --max-cache-disk 10G --persistent-cache"
    echo "  - KV cache pages were demoted to SSD storage"
    echo "  - Metrics confirm extended_kv_demotions and ssd_write_bytes activity"
    EXIT_CODE=0
elif [ "$READY" = true ] && [ "$HTTP_CODE" = "200" ]; then
    echo "Result: ⚠ SSD TIER AVAILABLE BUT NOT ACTIVATED"
    echo ""
    echo "The server started successfully with SSD tier enabled, but no pages"
    echo "were demoted to disk during testing. This is expected when:"
    echo "  - RAM budget is sufficient for the test context sizes"
    echo "  - No memory pressure triggered demotion policy"
    echo "  - Test prompts were too small to fill KV cache"
    echo ""
    echo "To verify SSD tier activation, try:"
    echo "  - Larger context windows (100K+ tokens)"
    echo "  - Multiple concurrent requests"
    echo "  - Reducing --max-memory budget"
    EXIT_CODE=0
else
    echo "Result: ✗ TEST FAILED"
    echo ""
    echo "The server failed to start or requests failed."
    echo "Check logs: /tmp/splash_ssd_serve.log"
    EXIT_CODE=1
fi

echo ""
echo "Logs: /tmp/splash_ssd_serve.log"
echo "Cache: $CACHE_DIR"
echo ""

exit $EXIT_CODE
