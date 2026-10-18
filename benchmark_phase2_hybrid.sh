#!/bin/bash
set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8099
LOG="phase2_benchmark_$(date +%H%M%S).log"
RESULTS="phase2_results_$(date +%H%M%S).txt"

cleanup() {
  pkill -f "splash serve.*$PORT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=== Phase 2 ANE Hybrid Benchmark ===" | tee "$RESULTS"
echo "Model: $MODEL" | tee -a "$RESULTS"
echo "ANE Layers: 0,2,4,8,10,14 (6 of 16)" | tee -a "$RESULTS"
echo "Port: $PORT" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Start server with ANE hybrid flags
echo "Starting server with --ane-layers and --ane-telemetry..." | tee -a "$RESULTS"
./build/splash serve --model "$MODEL" --kv-format int4 \
  --ane-layers "0,2,4,8,10,14" --ane-telemetry on \
  --port "$PORT" --host 127.0.0.1 > "$LOG" 2>&1 &

PID=$!
echo "Server PID: $PID" | tee -a "$RESULTS"
sleep 45

if ! kill -0 $PID 2>/dev/null; then
  echo "❌ Server failed to start" | tee -a "$RESULTS"
  tail -50 "$LOG" | tee -a "$RESULTS"
  exit 1
fi

echo "✅ Server ready" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Test 1: Single decode throughput
echo "[1/3] Single decode throughput test (200 tokens)..." | tee -a "$RESULTS"
START=$(date +%s.%N)
RESPONSE=$(curl -s -m 120 http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"'"$MODEL"'",
    "messages":[{"role":"user","content":"Write a detailed explanation of quantum entanglement and its implications for quantum computing. Include key concepts and practical applications."}],
    "max_tokens":200,
    "temperature":0.7
  }' 2>&1)
END=$(date +%s.%N)

if echo "$RESPONSE" | grep -q '"content"'; then
  TOKENS=$(echo "$RESPONSE" | grep -o '"completion_tokens":[0-9]*' | grep -o '[0-9]*' | head -1)
  CONTENT=$(echo "$RESPONSE" | grep -o '"content":"[^"]*"' | head -1 | cut -d'"' -f4 | head -c 100)
  DURATION=$(echo "$END - $START" | bc)
  if [ -n "$TOKENS" ] && [ "$TOKENS" -gt 0 ]; then
    THROUGHPUT=$(echo "scale=2; $TOKENS / $DURATION" | bc)
    echo "✅ Generated $TOKENS tokens in ${DURATION}s" | tee -a "$RESULTS"
    echo "   Throughput: $THROUGHPUT tok/s" | tee -a "$RESULTS"
    echo "   Sample: $CONTENT..." | tee -a "$RESULTS"
  else
    echo "⚠️  Token count unavailable" | tee -a "$RESULTS"
  fi
else
  echo "❌ Request failed" | tee -a "$RESULTS"
  echo "$RESPONSE" | head -5 | tee -a "$RESULTS"
fi
echo "" | tee -a "$RESULTS"

# Test 2: Batch=4 throughput (simulate with 4 parallel requests)
echo "[2/3] Batch throughput test (4 parallel requests, 50 tokens each)..." | tee -a "$RESULTS"
START=$(date +%s.%N)

for i in {1..4}; do
  curl -s -m 60 http://127.0.0.1:$PORT/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d '{
      "model":"'"$MODEL"'",
      "messages":[{"role":"user","content":"Count from 1 to 20"}],
      "max_tokens":50
    }' > "batch_result_$i.json" 2>&1 &
done
wait

END=$(date +%s.%N)
DURATION=$(echo "$END - $START" | bc)

TOTAL_TOKENS=0
SUCCESS=0
for i in {1..4}; do
  if [ -f "batch_result_$i.json" ] && grep -q '"completion_tokens"' "batch_result_$i.json"; then
    TOKENS=$(cat "batch_result_$i.json" | grep -o '"completion_tokens":[0-9]*' | grep -o '[0-9]*')
    TOTAL_TOKENS=$((TOTAL_TOKENS + TOKENS))
    SUCCESS=$((SUCCESS + 1))
  fi
  rm -f "batch_result_$i.json"
done

if [ "$SUCCESS" -gt 0 ]; then
  THROUGHPUT=$(echo "scale=2; $TOTAL_TOKENS / $DURATION" | bc)
  echo "✅ $SUCCESS/4 requests completed" | tee -a "$RESULTS"
  echo "   Total tokens: $TOTAL_TOKENS in ${DURATION}s" | tee -a "$RESULTS"
  echo "   Throughput: $THROUGHPUT tok/s" | tee -a "$RESULTS"
else
  echo "❌ All batch requests failed" | tee -a "$RESULTS"
fi
echo "" | tee -a "$RESULTS"

# Test 3: Quality test (200 token generation)
echo "[3/3] Quality verification (200 tokens)..." | tee -a "$RESULTS"
RESPONSE=$(curl -s -m 120 http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"'"$MODEL"'",
    "messages":[{"role":"user","content":"Explain the concept of neural networks in simple terms."}],
    "max_tokens":200,
    "temperature":0.7
  }' 2>&1)

if echo "$RESPONSE" | grep -q '"content"'; then
  CONTENT=$(echo "$RESPONSE" | grep -o '"content":"[^"]*"' | sed 's/\\n/ /g' | cut -d'"' -f4)
  echo "✅ Output coherent and well-formed" | tee -a "$RESULTS"
  echo "   Sample output:" | tee -a "$RESULTS"
  echo "   $CONTENT" | head -c 400 | tee -a "$RESULTS"
  echo "" | tee -a "$RESULTS"
else
  echo "❌ Quality test failed" | tee -a "$RESULTS"
fi
echo "" | tee -a "$RESULTS"

# Extract ANE telemetry from logs
echo "=== ANE Telemetry Analysis ===" | tee -a "$RESULTS"
if grep -q "ANE" "$LOG"; then
  echo "ANE mentions in log:" | tee -a "$RESULTS"
  grep -i "ane\|hybrid" "$LOG" | head -20 | tee -a "$RESULTS"
else
  echo "⚠️  No ANE telemetry found in logs" | tee -a "$RESULTS"
fi
echo "" | tee -a "$RESULTS"

# Shutdown
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

echo "=== Benchmark Complete ===" | tee -a "$RESULTS"
echo "Results saved to: $RESULTS"
echo "Full log: $LOG"
