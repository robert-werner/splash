#!/bin/bash
set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081

# Kill existing instance
lsof -ti:$PORT 2>/dev/null | xargs -r kill -9 || true
sleep 2

# Start server
./splash serve --model "$MODEL" --port $PORT --kv-format int4 > /tmp/attention_bench.log 2>&1 &
SERVER_PID=$!

# Wait for readiness
echo "Waiting for server startup..."
for i in {1..60}; do
  if curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
    echo "Server ready after ${i}s"
    break
  fi
  sleep 1
done

if ! curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
  echo "Server failed to start"
  kill $SERVER_PID 2>/dev/null || true
  exit 1
fi

# Prefill benchmark: 83K tokens
echo "=== Prefill benchmark (83K tokens) ==="
PROMPT=$(python3 -c "print('Test ' * 20750)")
time curl -s http://localhost:$PORT/v1/completions \
  -H "Content-Type: application/json" \
  -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" \
  | jq -r '.usage // {}'

# Decode benchmark: batch 1
echo -e "\n=== Decode benchmark (batch 1) ==="
time curl -s http://localhost:$PORT/v1/completions \
  -H "Content-Type: application/json" \
  -d '{"model": "'$MODEL'", "prompt": "Hello", "max_tokens": 50, "temperature": 0.7}' \
  | jq -r '.usage.total_time_ms // "N/A"'

# Decode benchmark: batch 4 (parallel)
echo -e "\n=== Decode benchmark (batch 4, parallel) ==="
(
  for i in {1..4}; do
    (curl -s http://localhost:$PORT/v1/completions \
      -H "Content-Type: application/json" \
      -d "{\"model\": \"$MODEL\", \"prompt\": \"Request $i\", \"max_tokens\": 30, \"temperature\": 0.7}" \
      > /tmp/req_$i.json) &
  done
  wait
)
for i in {1..4}; do
  echo "Request $i: $(jq -r '.usage.total_time_ms // "N/A"' /tmp/req_$i.json)ms"
done

# Cleanup
kill $SERVER_PID 2>/dev/null || true
echo -e "\n=== Server log tail ==="
tail -20 /tmp/attention_bench.log
