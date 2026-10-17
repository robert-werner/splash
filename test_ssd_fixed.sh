#!/bin/bash
# Test SSD-backed KV cache extension with progressively larger contexts

set -e

MODEL_DIR="$HOME/.cache/huggingface/hub/models--prism-ml--Ternary-Bonsai-2-27B-gguf/snapshots/b072e1d3b35a0a630cece372c2127528e0994386"
PORT=8081
MAX_CONTEXT=262144        # 262K tokens
MAX_MEMORY=20401094656    # 19 GB RAM budget (19 * 1024^3)
MAX_CACHE_DISK=10737418240 # 10 GB SSD tier (10 * 1024^3)

echo "=== Testing SSD tier functionality ==="
echo "Model: $MODEL_DIR"
echo "Port: $PORT"
echo "Max context: $MAX_CONTEXT"
echo "Max memory: $MAX_MEMORY"
echo "Max cache disk: $MAX_CACHE_DISK"
echo ""

# Start server with SSD tier enabled
echo "Starting server with SSD tier..."
./build/splash serve-native \
  "$MODEL_DIR" \
  "$MAX_CONTEXT" \
  "$MAX_MEMORY" \
  "$MAX_CACHE_DISK" \
  --state-format bf16 \
  --persistent-cache on \
  > /tmp/splash_ssd_test.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start
echo "Waiting for server to start (30 seconds)..."
sleep 30

# Check if server is still running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server process died during startup"
    echo "Last 50 lines of log:"
    tail -50 /tmp/splash_ssd_test.log
    exit 1
fi

# Test 1: Check server health
echo ""
echo "Test 1: Server health check..."
HEALTH_RESPONSE=$(curl -s http://127.0.0.1:$PORT/health || echo "FAILED")
if [[ "$HEALTH_RESPONSE" == "FAILED" ]]; then
    echo "ERROR: Health check failed"
    kill $SERVER_PID 2>/dev/null || true
    tail -50 /tmp/splash_ssd_test.log
    exit 1
fi
echo "$HEALTH_RESPONSE" | jq '.'

# Test 2: Small context (should stay in RAM)
echo ""
echo "Test 2: Small context (10K tokens, should stay in RAM)..."
PROMPT=$(python3 -c "print('token ' * 10000)")
USAGE_RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize in one sentence.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }" | jq '.usage')
echo "$USAGE_RESPONSE"

# Get metrics after small context
echo ""
echo "Metrics after small context test:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "(kv_|ssd_|extended_)" | head -30

# Test 3: Medium context (should trigger SSD tier)
echo ""
echo "Test 3: Medium context (50K tokens, may use SSD)..."
PROMPT=$(python3 -c "print('token ' * 50000)")
USAGE_RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize in one sentence.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }" | jq '.usage')
echo "$USAGE_RESPONSE"

# Test 4: Check final metrics
echo ""
echo "Test 4: Final server metrics after all tests..."
curl -s http://127.0.0.1:$PORT/metrics | grep -E "(kv_|ssd_|extended_)" | head -30

# Cleanup
echo ""
echo "Stopping server..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== SSD tier test complete ==="
echo "Full logs available at: /tmp/splash_ssd_test.log"
