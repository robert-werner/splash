#!/bin/bash
# Test SSD-backed KV cache extension with progressively larger contexts

set -e

MODEL_DIR="/Users/a1111/development/splash_of_mine/install/models/.resolved/147a396cdf6c78c1d3d033939353ab08fb4ae41a39e54d52d6d10d254b5aacb0"
PORT=8081

echo "=== Testing SSD tier functionality ==="
echo "Model: $MODEL_DIR"
echo "Port: $PORT"
echo ""

# Start server with SSD tier enabled
# serve-native MODEL_DIRECTORY MAX_CONTEXT MAX_MEMORY_BYTES MAX_CACHE_DISK_BYTES [options]
echo "Starting server with SSD tier..."
./build/splash serve-native \
  "$MODEL_DIR" \
  188409 \
  20401094656 \
  10737418240 \
  --state-format bf16 \
  --persistent-cache on &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start
sleep 15

# Test 1: Check server health
echo ""
echo "Test 1: Server health check..."
curl -s http://127.0.0.1:$PORT/health | jq '.'

# Test 2: Small context (should stay in RAM)
echo ""
echo "Test 2: Small context (10K tokens, should stay in RAM)..."
PROMPT=$(python3 -c "print('token ' * 10000)")
curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"splash\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize.\"}],
    \"max_tokens\": 100,
    \"stream\": false
  }" | jq '.usage'

# Test 3: Medium context (should trigger SSD tier)
echo ""
echo "Test 3: Medium context (50K tokens, may use SSD)..."
PROMPT=$(python3 -c "print('token ' * 50000)")
curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"splash\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize.\"}],
    \"max_tokens\": 100,
    \"stream\": false
  }" | jq '.usage'

# Test 4: Check metrics
echo ""
echo "Test 4: Server metrics after tests..."
curl -s http://127.0.0.1:$PORT/metrics | grep -E "kv_|ssd_|extended_" | head -20

# Cleanup
echo ""
echo "Stopping server..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== SSD tier test complete ==="
