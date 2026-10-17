#!/bin/bash
set -euo pipefail

# Simple attention baseline from server logs
MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8083

lsof -ti:$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
sleep 1

echo "Starting server for baseline measurement..."
./splash serve --model "$MODEL" --port $PORT --kv-format int4 > /tmp/attn_base.log 2>&1 &
SERVER_PID=$!

for i in {1..60}; do
  if curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
    echo "Server ready"
    break
  fi
  sleep 1
done

if ! curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
  echo "Server failed"
  kill $SERVER_PID 2>/dev/null || true
  exit 1
fi

echo ""
echo "=== Baseline: 1K prompt ==="
for run in 1 2 3; do
  PROMPT=$(python3 -c "print('Test ' * 256)")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  sleep 0.5
done

echo ""
echo "=== Baseline: 2K prompt ==="
for run in 1 2 3; do
  PROMPT=$(python3 -c "print('Test ' * 512)")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  sleep 0.5
done

echo ""
echo "=== Baseline: 4K prompt ==="
for run in 1 2 3; do
  PROMPT=$(python3 -c "print('Test ' * 1024)")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  sleep 0.5
done

sleep 2
kill $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Log output ==="
grep "TTFT" /tmp/attn_base.log | tail -12
