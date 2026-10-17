#!/bin/bash
set -euo pipefail

# Baseline attention performance measurement
# Measures prefill attention with varying prompt lengths

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8082

# Kill existing
lsof -ti:$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
sleep 1

# Start server
echo "Starting server..."
./splash serve --model "$MODEL" --port $PORT --kv-format int4 > /tmp/attention_baseline.log 2>&1 &
SERVER_PID=$!

# Wait for ready
for i in {1..60}; do
  if curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
    echo "Server ready"
    break
  fi
  sleep 1
done

if ! curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
  echo "Server failed to start"
  kill $SERVER_PID 2>/dev/null || true
  exit 1
fi

# Benchmark different prompt lengths
for tokens in 1024 2048 4096 8192; do
  echo ""
  echo "=== Prefill: $tokens tokens ==="
  
  # Generate prompt
  PROMPT=$(python3 -c "print('Test ' * $(($tokens / 4)))")
  
  # 3 runs for stability
  for run in 1 2 3; do
    RESULT=$(curl -s http://localhost:$PORT/v1/completions \
      -H "Content-Type: application/json" \
      -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" \
      | jq -r '.usage.prompt_tokens, .usage.total_time_ms')
    
    ACTUAL_TOKENS=$(echo "$RESULT" | head -1)
    TIME_MS=$(echo "$RESULT" | tail -1)
    
    if [ "$TIME_MS" != "null" ] && [ -n "$TIME_MS" ]; then
      TOKS_PER_SEC=$(python3 -c "print(f'{$ACTUAL_TOKENS / ($TIME_MS / 1000.0):.1f}')")
      echo "Run $run: $ACTUAL_TOKENS tokens in ${TIME_MS}ms = ${TOKS_PER_SEC} tok/s"
    else
      echo "Run $run: Failed"
    fi
  done
done

# Cleanup
kill $SERVER_PID 2>/dev/null || true
echo ""
echo "=== Baseline complete ==="
