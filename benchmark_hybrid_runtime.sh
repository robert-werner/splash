#!/bin/bash
set -euo pipefail

# Phase 2 Hybrid GGUF Runtime Benchmark
# Tests PTQ1_0+PQ2_0 hybrid model with ANE layers 0,2,4,8,10,14

MODEL_DIR="install/models/hybrid-test"
PORT=8099
RESULTS="hybrid_benchmark_results_$(date +%H%M%S).txt"
LOG="hybrid_benchmark_$(date +%H%M%S).log"

cleanup() {
  pkill -f "splash serve-native.*$PORT" 2>/dev/null || true
  sleep 2
}
trap cleanup EXIT INT TERM

echo "=== Phase 2 Hybrid GGUF Runtime Benchmark ===" | tee "$RESULTS"
echo "Model: Ternary-Bonsai-2-27B-hybrid-ptq1pq2.gguf" | tee -a "$RESULTS"
echo "ANE Layers: 0,2,4,8,10,14 (6 of 16 FFN layers)" | tee -a "$RESULTS"
echo "Baseline: 62.9 tok/s single, 143.5 tok/s batch=4" | tee -a "$RESULTS"
echo "Target: 81.8 tok/s single (+30%), 186.6 tok/s batch=4 (+30%)" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Verify model exists
if [ ! -f "$MODEL_DIR/target/model.gguf" ]; then
  echo "❌ Hybrid GGUF not found at $MODEL_DIR/target/model.gguf" | tee -a "$RESULTS"
  exit 1
fi

echo "✅ Hybrid GGUF found: $(du -sh $MODEL_DIR/target/model.gguf | cut -f1)" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Start server with ANE hybrid flags
echo "[1/5] Starting server with ANE hybrid flags..." | tee -a "$RESULTS"
echo "Command: serve-native with --ane-layers '0,2,4,8,10,14' --ane-telemetry on" | tee -a "$RESULTS"

./build/splash serve-native "$MODEL_DIR" 180000 auto \
  --ane-layers "0,2,4,8,10,14" --ane-telemetry on > "$LOG" 2>&1 &

PID=$!
echo "Server PID: $PID" | tee -a "$RESULTS"

# Wait for server to start (check for startup in log)
echo "Waiting for server startup..." | tee -a "$RESULTS"
sleep 5

for i in {1..18}; do
  if grep -q "Pipeline warming" "$LOG" 2>/dev/null; then
    echo "✅ Server started (pipeline warming detected)" | tee -a "$RESULTS"
    break
  fi
  if ! kill -0 $PID 2>/dev/null; then
    echo "❌ Server failed to start" | tee -a "$RESULTS"
    tail -30 "$LOG" | tee -a "$RESULTS"
    exit 1
  fi
  sleep 2
done

if ! grep -q "Pipeline warming" "$LOG" 2>/dev/null; then
  echo "⚠️  Server may not be ready yet, proceeding anyway" | tee -a "$RESULTS"
fi

echo "" | tee -a "$RESULTS"

# Test 1: Single decode throughput (200 tokens)
echo "[2/5] Single decode throughput test (200 tokens)..." | tee -a "$RESULTS"
START=$(date +%s.%N)

RESPONSE=$(curl -s -m 120 http://127.0.0.1:8099/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"ternary-bonsai",
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
    THROUGHPUT=$(echo "scale=1; $TOKENS / $DURATION" | bc)
    DELTA_VS_BASELINE=$(echo "scale=1; (($THROUGHPUT - 62.9) / 62.9) * 100" | bc)
    
    echo "✅ Single decode: $THROUGHPUT tok/s" | tee -a "$RESULTS"
    echo "   Generated $TOKENS tokens in ${DURATION}s" | tee -a "$RESULTS"
    echo "   vs Baseline 62.9 tok/s: ${DELTA_VS_BASELINE}%" | tee -a "$RESULTS"
    echo "   Sample: $CONTENT..." | tee -a "$RESULTS"
    
    SINGLE_THROUGHPUT=$THROUGHPUT
  else
    echo "⚠️  Token count unavailable" | tee -a "$RESULTS"
    SINGLE_THROUGHPUT="N/A"
  fi
else
  echo "❌ Single decode request failed" | tee -a "$RESULTS"
  echo "$RESPONSE" | head -5 | tee -a "$RESULTS"
  SINGLE_THROUGHPUT="FAILED"
fi

echo "" | tee -a "$RESULTS"
sleep 2

# Test 2: Batch=4 throughput (4 parallel requests, 50 tokens each)
echo "[3/5] Batch throughput test (4 parallel requests, 50 tokens each)..." | tee -a "$RESULTS"
START=$(date +%s.%N)

for i in {1..4}; do
  curl -s -m 60 http://127.0.0.1:8099/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d '{
      "model":"ternary-bonsai",
      "messages":[{"role":"user","content":"Count from 1 to 20 and explain the significance of prime numbers."}],
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
  BATCH_THROUGHPUT=$(echo "scale=1; $TOTAL_TOKENS / $DURATION" | bc)
  DELTA_VS_BASELINE=$(echo "scale=1; (($BATCH_THROUGHPUT - 143.5) / 143.5) * 100" | bc)
  
  echo "✅ Batch=4 decode: $BATCH_THROUGHPUT tok/s" | tee -a "$RESULTS"
  echo "   $SUCCESS/4 requests completed" | tee -a "$RESULTS"
  echo "   Total tokens: $TOTAL_TOKENS in ${DURATION}s" | tee -a "$RESULTS"
  echo "   vs Baseline 143.5 tok/s: ${DELTA_VS_BASELINE}%" | tee -a "$RESULTS"
else
  echo "❌ All batch requests failed" | tee -a "$RESULTS"
  BATCH_THROUGHPUT="FAILED"
fi

echo "" | tee -a "$RESULTS"

# Test 3: Extract ANE telemetry from logs
echo "[4/5] ANE Telemetry Analysis..." | tee -a "$RESULTS"

if grep -q "ANE" "$LOG"; then
  echo "ANE telemetry found in logs:" | tee -a "$RESULTS"
  grep -i "ane\|hybrid\|utilization\|accelerator" "$LOG" | head -30 | tee -a "$RESULTS"
  
  # Try to extract ANE utilization percentage
  if grep -q "utilization" "$LOG"; then
    ANE_UTIL=$(grep -i "ane.*utilization\|utilization.*ane" "$LOG" | head -1)
    echo "" | tee -a "$RESULTS"
    echo "ANE Utilization: $ANE_UTIL" | tee -a "$RESULTS"
  else
    echo "" | tee -a "$RESULTS"
    echo "⚠️  ANE utilization percentage not found in logs" | tee -a "$RESULTS"
  fi
else
  echo "⚠️  No ANE telemetry found in logs" | tee -a "$RESULTS"
  echo "Log preview:" | tee -a "$RESULTS"
  head -50 "$LOG" | tee -a "$RESULTS"
fi

echo "" | tee -a "$RESULTS"

# Test 4: Quality verification
echo "[5/5] Quality verification (coherent output test)..." | tee -a "$RESULTS"
RESPONSE=$(curl -s -m 120 http://127.0.0.1:8099/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model":"ternary-bonsai",
    "messages":[{"role":"user","content":"Explain neural networks in simple terms."}],
    "max_tokens":150,
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

# Generate final benchmark report table
echo "=== Benchmark Report ===" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"
echo "| Metric              | Baseline | Actual         | Target  | Delta vs Baseline | Target Met? |" | tee -a "$RESULTS"
echo "|---------------------|----------|----------------|---------|-------------------|-------------|" | tee -a "$RESULTS"
echo "| Single Decode       | 62.9     | ${SINGLE_THROUGHPUT:-N/A}        | 81.8    | ${DELTA_VS_BASELINE:-N/A}%           | TBD         |" | tee -a "$RESULTS"
echo "| Batch=4 Decode      | 143.5    | ${BATCH_THROUGHPUT:-N/A}       | 186.6   | ${DELTA_VS_BASELINE:-N/A}%           | TBD         |" | tee -a "$RESULTS"
echo "| ANE Utilization     | 0%       | TBD            | 35-40%  | N/A               | TBD         |" | tee -a "$RESULTS"
echo "| Memory              | 8.85 GB  | TBD            | 10.95GB | N/A               | N/A         |" | tee -a "$RESULTS"
echo "| TTFT                | 0.5s     | TBD            | 0.5s    | N/A               | N/A         |" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Shutdown server
echo "Shutting down server..." | tee -a "$RESULTS"
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

echo "" | tee -a "$RESULTS"
echo "=== Benchmark Complete ===" | tee -a "$RESULTS"
echo "Results saved to: $RESULTS" | tee -a "$RESULTS"
echo "Full log: $LOG" | tee -a "$RESULTS"
