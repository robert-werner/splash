#!/bin/bash
# Comprehensive test of Phase 1, 3, 4 optimizations on prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0
# Phase 2 (ANE hybrid) blocked on GGUF loader per-layer quantization (requires runtime conversion)

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8095
HOST="127.0.0.1"
BASE_URL="http://${HOST}:${PORT}"
LOG_DIR="test_logs_$(date +%Y%m%d_%H%M%S)"

mkdir -p "${LOG_DIR}"

echo "=== Phase Optimizations Test Suite ==="
echo "Model: ${MODEL}"
echo "Logs: ${LOG_DIR}/"
echo ""

# Kill any existing splash processes
pkill -9 splash 2>/dev/null || true
sleep 2

# Helper: start server with options
start_server() {
  local name="$1"
  shift
  local log="${LOG_DIR}/${name}.log"
  
  echo "Starting: ${name}"
  ./splash serve --model "${MODEL}" --kv-format int4 --vision-text-only \
    --port "${PORT}" --host "${HOST}" "$@" > "${log}" 2>&1 &
  local pid=$!
  
  # Wait for server ready or failure (max 30s)
  for i in {1..60}; do
    if grep -q "Ready" "${log}" 2>/dev/null; then
      echo "  ✅ Ready in $((i/2))s"
      return 0
    fi
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "  ❌ Failed to start"
      cat "${log}"
      return 1
    fi
    sleep 0.5
  done
  
  echo "  ❌ Timeout waiting for Ready"
  kill -9 "${pid}" 2>/dev/null || true
  return 1
}

# Helper: send request and measure TTFT + throughput
benchmark_request() {
  local prompt="$1"
  local output="$2"
  
  local start=$(date +%s.%N)
  
  curl -s -X POST "${BASE_URL}/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d "{
      \"model\": \"${MODEL}\",
      \"messages\": [{\"role\": \"user\", \"content\": \"${prompt}\"}],
      \"max_tokens\": 150,
      \"temperature\": 0.7,
      \"stream\": false
    }" > "${output}" 2>&1
  
  local end=$(date +%s.%N)
  local duration=$(echo "$end - $start" | bc)
  
  # Extract completion tokens from response
  local tokens=$(jq -r '.usage.completion_tokens // 0' "${output}" 2>/dev/null || echo "0")
  local throughput=0
  if [ "${tokens}" -gt 0 ] && [ "$(echo "${duration} > 0" | bc)" -eq 1 ]; then
    throughput=$(echo "scale=2; ${tokens} / ${duration}" | bc)
  fi
  
  echo "${duration},${tokens},${throughput}"
}

# Test 1: Baseline (no optimizations)
echo ""
echo "=== Test 1: Baseline (current state) ==="
if start_server "baseline"; then
  sleep 2
  
  # First request (cold start - measures TTFT with Phase 1 shader warming)
  echo "  First request (cold)..."
  metrics=$(benchmark_request "Write a short poem about recursion in programming." \
    "${LOG_DIR}/baseline_cold_response.json")
  IFS=',' read -r duration tokens throughput <<< "${metrics}"
  echo "    Duration: ${duration}s, Tokens: ${tokens}, Throughput: ${throughput} tok/s"
  
  # Second request (warm)
  sleep 1
  echo "  Second request (warm)..."
  metrics=$(benchmark_request "Explain quantum entanglement in simple terms." \
    "${LOG_DIR}/baseline_warm_response.json")
  IFS=',' read -r duration tokens throughput <<< "${metrics}"
  echo "    Duration: ${duration}s, Tokens: ${tokens}, Throughput: ${throughput} tok/s"
  
  pkill -9 splash 2>/dev/null || true
  sleep 2
else
  echo "  ⚠️  Baseline test failed"
fi

# Test 2: Phase 3 (Async KV Swap) - extended context
echo ""
echo "=== Test 2: Phase 3 - Async KV Swap (1M+ context) ==="
echo "  ⚠️  Note: --enable-async-kv-swap CLI flag not yet wired in NativeArguments.cpp"
echo "  Skipping until CLI integration complete"

# Test 3: Phase 4 (Weight Prefetch) - decode latency optimization
echo ""
echo "=== Test 3: Phase 4 - Weight Prefetch (decode optimization) ==="
echo "  ⚠️  Note: --enable-weight-prefetch CLI flag not yet wired in NativeArguments.cpp"
echo "  Skipping until CLI integration complete"

# Test 4: Check Phase 1 shader warming in logs
echo ""
echo "=== Test 4: Phase 1 - Shader Cache Warming ==="
if grep -q "Pipeline warming:" "${LOG_DIR}/baseline.log"; then
  warming_time=$(grep "Pipeline warming:" "${LOG_DIR}/baseline.log" | awk '{print $NF}')
  echo "  ✅ Shader warming active: ${warming_time}"
  echo "  Expected: eliminates 2-3s TTFT penalty on first request"
else
  echo "  ❌ Shader warming not found in logs"
fi

# Extract key metrics from logs
echo ""
echo "=== Summary ==="
echo ""
echo "Phase 1 (Metal Shader Warming):"
if grep -q "Pipeline warming:" "${LOG_DIR}/baseline.log"; then
  echo "  ✅ Active - warming time: $(grep "Pipeline warming:" "${LOG_DIR}/baseline.log" | awk '{print $NF}')"
else
  echo "  ❌ Not detected"
fi

echo ""
echo "Phase 2 (ANE Hybrid Quantization):"
echo "  ⚠️  Blocked: requires per-layer GGUF loading (runtime PQ2_0→Q4_K conversion)"
echo "  Infrastructure ready: HybridQuantConfig, CLI parsing, validation"
echo "  Remaining: GgufTarget.cpp per-layer format selection (multi-day task)"

echo ""
echo "Phase 3 (Async KV Swap):"
echo "  ⚠️  Core ready: AsyncKvSwap.{hpp,cpp} implemented"
echo "  Remaining: CLI flags wiring in NativeArguments.cpp"

echo ""
echo "Phase 4 (Weight Prefetch):"
echo "  ⚠️  Core ready: WeightPrefetch.{hpp,cpp} implemented"
echo "  Remaining: CLI flags wiring in NativeArguments.cpp"

echo ""
echo "All logs saved to: ${LOG_DIR}/"
