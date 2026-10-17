#!/bin/bash
# Test SSD-backed KV cache extension with progressively larger contexts

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081

echo "=== Testing SSD tier functionality ==="
echo "Model: $MODEL"
echo "Port: $PORT"
echo ""

# Start server with SSD tier enabled
echo "Starting server with SSD tier..."
./splash serve \
  --model "$MODEL" \
  --port $PORT \
  --max-memory 19G \
  --max-context 188409 \
  --state-format bf16 \
  --max-cache-disk 10G \
  --persistent-cache \
  --vision-text-only \
  --offline &

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
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize.\"}],
    \"max_tokens\": 100,
    \"stream\": false
  }" | jq '.usage'

# Check metrics after small context
echo ""
echo "Metrics after small context:"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "extended_kv|ssd_" | head -20

# Test 3: Medium context (should trigger SSD tier)
echo ""
echo "Test 3: Medium context (30K tokens, may use SSD)..."
PROMPT=$(python3 -c "print('token ' * 30000)")
curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
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
