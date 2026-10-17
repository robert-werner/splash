#!/bin/bash
# Test SSD-backed KV cache extension using Python server

set -e

MODEL_REPO="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081
WORK_DIR="/tmp/splash_test_model"

echo "=== Testing SSD tier functionality with Python server ==="
echo "Model: $MODEL_REPO"
echo "Port: $PORT"
echo "Work dir: $WORK_DIR"
echo ""

# Create work directory
mkdir -p "$WORK_DIR"

# Start server with SSD tier enabled
echo "Starting server with SSD tier..."
cd /Users/a1111/development/splash_of_mine
source .venv/bin/activate

python3 -m server.server \
  --model "$MODEL_REPO" \
  --tokenizer "$MODEL_REPO" \
  --port $PORT \
  --working-set 19G \
  --max-cache-disk 10G \
  --persistent-cache \
  --state-format bf16 \
  --vision-text-only \
  "$WORK_DIR" \
  > /tmp/splash_ssd_python.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start
echo "Waiting for server to start (60 seconds for model download/prep)..."
sleep 60

# Check if server is still running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server process died during startup"
    echo "Last 100 lines of log:"
    tail -100 /tmp/splash_ssd_python.log
    exit 1
fi

# Test 1: Check server health
echo ""
echo "Test 1: Server health check..."
for i in {1..5}; do
    HEALTH_RESPONSE=$(curl -s http://127.0.0.1:$PORT/health 2>/dev/null || echo "")
    if [[ -n "$HEALTH_RESPONSE" ]]; then
        echo "$HEALTH_RESPONSE" | jq '.' 2>/dev/null || echo "$HEALTH_RESPONSE"
        break
    fi
    echo "Attempt $i failed, waiting 10 more seconds..."
    sleep 10
done

if [[ -z "$HEALTH_RESPONSE" ]]; then
    echo "ERROR: Health check failed after retries"
    kill $SERVER_PID 2>/dev/null || true
    tail -100 /tmp/splash_ssd_python.log
    exit 1
fi

# Test 2: Get initial metrics
echo ""
echo "Initial metrics (before load):"
curl -s http://127.0.0.1:$PORT/metrics 2>/dev/null | grep -E "(kv_|ssd_|extended_)" | head -20 || echo "No SSD metrics yet"

# Test 3: Small context (should stay in RAM)
echo ""
echo "Test 2: Small context (10K tokens, should stay in RAM)..."
PROMPT=$(python3 -c "print('word ' * 10000)")
RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"${PROMPT:0:50000}\n\nSummarize the above in one sentence.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }" 2>/dev/null)

echo "$RESPONSE" | jq '.usage' 2>/dev/null || echo "Request completed (no JSON)"

# Metrics after small context
echo ""
echo "Metrics after small context:"
curl -s http://127.0.0.1:$PORT/metrics 2>/dev/null | grep -E "(kv_|ssd_|extended_)" | head -30

# Test 4: Medium context (should trigger SSD tier)
echo ""
echo "Test 3: Medium context (50K tokens, may trigger SSD)..."
PROMPT=$(python3 -c "print('word ' * 50000)")
RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"default\",
    \"messages\": [{\"role\": \"user\", \"content\": \"${PROMPT:0:250000}\n\nSummarize the above in one sentence.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }" 2>/dev/null)

echo "$RESPONSE" | jq '.usage' 2>/dev/null || echo "Request completed (no JSON)"

# Test 5: Final metrics
echo ""
echo "Final metrics after all tests:"
curl -s http://127.0.0.1:$PORT/metrics 2>/dev/null | grep -E "(kv_|ssd_|extended_)" 

# Cleanup
echo ""
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== SSD tier test complete ==="
echo "Full logs: /tmp/splash_ssd_python.log"
echo "Work dir: $WORK_DIR"
