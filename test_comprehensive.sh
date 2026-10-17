#!/bin/bash
# Comprehensive integration test for all optimizations
# Tests: --recalibrate-ane, --lazy-draft-loading, SSD tier, Week 1, Week 2

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081
TEST_LOG="comprehensive_test_results.log"

echo "=== Comprehensive Integration Test ===" | tee "$TEST_LOG"
echo "Model: $MODEL" | tee -a "$TEST_LOG"
echo "Date: $(date)" | tee -a "$TEST_LOG"
echo "" | tee -a "$TEST_LOG"

# Test 1: Baseline (no optimizations)
echo "Test 1: Baseline configuration" | tee -a "$TEST_LOG"
./build/splash serve \
  --model "$MODEL" \
  --port $PORT \
  --working-set 19G \
  --vision-text-only \
  --state-format bf16 \
  --max-batch-size 4 &
SERVER_PID=$!
echo "Server PID: $SERVER_PID" | tee -a "$TEST_LOG"
sleep 15

# Health check
curl -s http://127.0.0.1:$PORT/health | jq '.' | tee -a "$TEST_LOG"

# Simple generation test
echo "Testing baseline generation..." | tee -a "$TEST_LOG"
TIME_START=$(date +%s.%N)
RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"Count from 1 to 10.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }")
TIME_END=$(date +%s.%N)
LATENCY=$(echo "$TIME_END - $TIME_START" | bc)
echo "Baseline latency: ${LATENCY}s" | tee -a "$TEST_LOG"
echo "$RESPONSE" | jq '.usage' | tee -a "$TEST_LOG"

# Stop baseline server
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
sleep 5

# Test 2: With --recalibrate-ane --lazy-draft-loading
echo "" | tee -a "$TEST_LOG"
echo "Test 2: With --recalibrate-ane --lazy-draft-loading" | tee -a "$TEST_LOG"
./build/splash serve \
  --model "$MODEL" \
  --port $PORT \
  --working-set 19G \
  --vision-text-only \
  --state-format bf16 \
  --max-batch-size 4 \
  --recalibrate-ane \
  --lazy-draft-loading &
SERVER_PID=$!
echo "Server PID: $SERVER_PID" | tee -a "$TEST_LOG"
sleep 15

# Health check
curl -s http://127.0.0.1:$PORT/health | jq '.' | tee -a "$TEST_LOG"

# Generation test
echo "Testing with new flags..." | tee -a "$TEST_LOG"
TIME_START=$(date +%s.%N)
RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"Count from 1 to 10.\"}],
    \"max_tokens\": 50,
    \"stream\": false
  }")
TIME_END=$(date +%s.%N)
LATENCY=$(echo "$TIME_END - $TIME_START" | bc)
echo "New flags latency: ${LATENCY}s" | tee -a "$TEST_LOG"
echo "$RESPONSE" | jq '.usage' | tee -a "$TEST_LOG"

# Stop server
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
sleep 5

# Test 3: With SSD tier
echo "" | tee -a "$TEST_LOG"
echo "Test 3: With SSD tier (--max-cache-disk 10G)" | tee -a "$TEST_LOG"
./build/splash serve \
  --model "$MODEL" \
  --port $PORT \
  --working-set 19G \
  --vision-text-only \
  --state-format bf16 \
  --max-batch-size 4 \
  --max-cache-disk 10G \
  --persistent-cache &
SERVER_PID=$!
echo "Server PID: $SERVER_PID" | tee -a "$TEST_LOG"
sleep 15

# Health check
curl -s http://127.0.0.1:$PORT/health | jq '.' | tee -a "$TEST_LOG"

# Large context test
echo "Testing SSD tier with large context..." | tee -a "$TEST_LOG"
PROMPT=$(python3 -c "print('token ' * 50000)")
TIME_START=$(date +%s.%N)
RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d "{
    \"model\": \"$MODEL\",
    \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\nSummarize in one word.\"}],
    \"max_tokens\": 10,
    \"stream\": false
  }")
TIME_END=$(date +%s.%N)
LATENCY=$(echo "$TIME_END - $TIME_START" | bc)
echo "SSD tier latency (50K context): ${LATENCY}s" | tee -a "$TEST_LOG"
echo "$RESPONSE" | jq '.usage' | tee -a "$TEST_LOG"

# Check SSD metrics
echo "SSD tier metrics:" | tee -a "$TEST_LOG"
curl -s http://127.0.0.1:$PORT/metrics | grep -E "kv_|ssd_|extended_" | tee -a "$TEST_LOG"

# Stop server
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
sleep 5

# Test 4: All optimizations combined (if Week 1/2 are ready)
if ./build/splash serve --help 2>&1 | grep -q "kv-compression"; then
  echo "" | tee -a "$TEST_LOG"
  echo "Test 4: All optimizations combined" | tee -a "$TEST_LOG"
  ./build/splash serve \
    --model "$MODEL" \
    --port $PORT \
    --working-set 19G \
    --vision-text-only \
    --state-format bf16 \
    --max-batch-size 4 \
    --recalibrate-ane \
    --lazy-draft-loading \
    --max-cache-disk 10G \
    --persistent-cache \
    --kv-compression \
    --adaptive-batching \
    --prefill-chunk-size 8192 \
    --fused-attention \
    --persistent-ane-calibration \
    --speculation-threshold 0.25 &
  SERVER_PID=$!
  echo "Server PID: $SERVER_PID" | tee -a "$TEST_LOG"
  sleep 15

  # Health check
  curl -s http://127.0.0.1:$PORT/health | jq '.' | tee -a "$TEST_LOG"

  # Full test
  echo "Testing all optimizations..." | tee -a "$TEST_LOG"
  TIME_START=$(date +%s.%N)
  RESPONSE=$(curl -s http://127.0.0.1:$PORT/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d "{
      \"model\": \"$MODEL\",
      \"messages\": [{\"role\": \"user\", \"content\": \"What is 17 * 23?\"}],
      \"max_tokens\": 20,
      \"stream\": false
    }")
  TIME_END=$(date +%s.%N)
  LATENCY=$(echo "$TIME_END - $TIME_START" | bc)
  echo "Full optimization latency: ${LATENCY}s" | tee -a "$TEST_LOG"
  echo "$RESPONSE" | jq '.' | tee -a "$TEST_LOG"

  # Metrics
  echo "Combined metrics:" | tee -a "$TEST_LOG"
  curl -s http://127.0.0.1:$PORT/metrics | grep -E "compression_|adaptive_|chunking_|fused_|ane_|speculation_" | tee -a "$TEST_LOG"

  # Stop server
  kill $SERVER_PID
  wait $SERVER_PID 2>/dev/null || true
else
  echo "" | tee -a "$TEST_LOG"
  echo "Test 4: Skipped (Week 1/2 flags not yet available)" | tee -a "$TEST_LOG"
fi

echo "" | tee -a "$TEST_LOG"
echo "=== Comprehensive test complete ===" | tee -a "$TEST_LOG"
echo "Results saved to: $TEST_LOG" | tee -a "$TEST_LOG"
