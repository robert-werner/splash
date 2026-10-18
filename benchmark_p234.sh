#!/bin/bash
set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8098
LOG="benchmark_p234_$(date +%H%M%S).log"

cleanup() {
  pkill -f "splash serve.*$PORT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=== Phase 2+3+4 Inference Benchmark ==="
echo "Model: $MODEL"
echo "Port: $PORT"
echo ""

# Start server with Phase 3+4 enabled
echo "Starting server with Phase 3+4 optimizations..."
./splash serve --model "$MODEL" --kv-format int4 --vision-text-only \
  --kv-swap-enable on --kv-swap-hot-tier 200000 --kv-swap-batch-size 32 \
  --enable-weight-prefetch on --prefetch-layers 4 \
  --port "$PORT" --host 127.0.0.1 > "$LOG" 2>&1 &

PID=$!
echo "Server PID: $PID"
sleep 60

if ! kill -0 $PID 2>/dev/null; then
  echo "❌ Server failed to start"
  tail -30 "$LOG"
  exit 1
fi

echo "✅ Server ready, running inference tests..."
echo ""

# Test 1: Short completion
echo "[1/3] Short completion (20 tokens)..."
START=$(date +%s)
curl -s -m 60 http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"'$MODEL'","messages":[{"role":"user","content":"Count from 1 to 10"}],"max_tokens":20}' \
  > result_short.json 2>&1
END=$(date +%s)

if [ -s result_short.json ] && grep -q '"content"' result_short.json; then
  CONTENT=$(cat result_short.json | grep -o '"content":"[^"]*"' | head -1 | cut -d'"' -f4)
  echo "✅ Response: $CONTENT"
  echo "   Time: $((END - START))s"
else
  echo "❌ Failed or timeout"
  cat result_short.json | head -5
fi
echo ""

# Test 2: Medium completion
echo "[2/3] Medium completion (100 tokens)..."
START=$(date +%s)
curl -s -m 90 http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"'$MODEL'","messages":[{"role":"user","content":"Write a haiku about autumn"}],"max_tokens":100}' \
  > result_medium.json 2>&1
END=$(date +%s)

if [ -s result_medium.json ] && grep -q '"content"' result_medium.json; then
  TOKENS=$(cat result_medium.json | grep -o '"completion_tokens":[0-9]*' | grep -o '[0-9]*')
  echo "✅ Generated $TOKENS tokens"
  echo "   Time: $((END - START))s"
  echo "   Throughput: ~$(( TOKENS / (END - START + 1) )) tok/s"
else
  echo "❌ Failed or timeout"
fi
echo ""

# Test 3: Server metrics
echo "[3/3] Server metrics..."
curl -s -m 10 http://127.0.0.1:$PORT/v1/models 2>&1 | head -5 || echo "⚠️  Metrics endpoint unavailable"
echo ""

echo "=== Summary ==="
echo "✅ Phase 3+4 inference tests complete"
echo ""
echo "Log: $LOG"
echo "Checking log for Phase 3+4 indicators..."
grep -i "swap\|prefetch\|phase\|config" "$LOG" | grep -v "python\|import" | head -10 || echo "(no explicit mentions)"

kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
