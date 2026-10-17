#!/usr/bin/env bash
#
# Test context-aware priority scheduling vs baseline.
# Measures p50/p95/p99 latency for mixed workload.

set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081
DURATION=90  # seconds per test
WARMUP=10    # warmup period

echo "==================================================================="
echo "Context-Aware Scheduling Benchmark"
echo "==================================================================="
echo ""

# Kill any existing splash process
cleanup() {
    echo "Cleaning up..."
    lsof -ti:$PORT | xargs -r kill -9 2>/dev/null || true
    sleep 2
}

trap cleanup EXIT

# Function to wait for server readiness
wait_for_ready() {
    local max_wait=60
    local waited=0
    echo "Waiting for server readiness..."
    
    while [ $waited -lt $max_wait ]; do
        if curl -s "http://localhost:$PORT/v1/models" >/dev/null 2>&1; then
            echo "Server ready!"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    
    echo "Error: Server did not become ready in ${max_wait}s"
    return 1
}

# Test 1: Baseline (no priority scheduling)
echo ""
echo "==================================================================="
echo "TEST 1: Baseline (FIFO scheduling, no priorities)"
echo "==================================================================="
cleanup

echo "Starting splash without priority scheduling..."
./splash serve --model "$MODEL" --port $PORT --kv-format int4 > /tmp/splash_baseline.log 2>&1 &
SPLASH_PID=$!

if ! wait_for_ready; then
    cat /tmp/splash_baseline.log
    exit 1
fi

echo "Warmup period: ${WARMUP}s"
sleep $WARMUP

echo "Running baseline benchmark..."
python3 tools/measure_mixed_latency.py \
    --port $PORT \
    --duration $DURATION \
    --interactive-rate 2.0 \
    --batch-rate 0.5 \
    > /tmp/baseline_results.txt

cat /tmp/baseline_results.txt

cleanup

# Test 2: With priority scheduling
echo ""
echo "==================================================================="
echo "TEST 2: Context-Aware Priority Scheduling"
echo "==================================================================="

echo "Starting splash WITH priority scheduling..."
./splash serve --model "$MODEL" --port $PORT --kv-format int4 \
    --priority-scheduling \
    --preempt-threshold 4096 \
    > /tmp/splash_priority.log 2>&1 &
SPLASH_PID=$!

if ! wait_for_ready; then
    cat /tmp/splash_priority.log
    exit 1
fi

echo "Warmup period: ${WARMUP}s"
sleep $WARMUP

echo "Running priority scheduling benchmark..."
python3 tools/measure_mixed_latency.py \
    --port $PORT \
    --duration $DURATION \
    --interactive-rate 2.0 \
    --batch-rate 0.5 \
    > /tmp/priority_results.txt

cat /tmp/priority_results.txt

cleanup

# Compare results
echo ""
echo "==================================================================="
echo "COMPARISON: Baseline vs Priority Scheduling"
echo "==================================================================="
echo ""
echo "BASELINE RESULTS:"
grep -A 10 "INTERACTIVE" /tmp/baseline_results.txt || echo "No baseline results"
echo ""
echo "PRIORITY SCHEDULING RESULTS:"
grep -A 10 "INTERACTIVE" /tmp/priority_results.txt || echo "No priority results"
echo ""
echo "Full logs:"
echo "  Baseline:  /tmp/splash_baseline.log"
echo "  Priority:  /tmp/splash_priority.log"
echo "  Baseline metrics: /tmp/baseline_results.txt"
echo "  Priority metrics: /tmp/priority_results.txt"
echo ""
echo "==================================================================="
echo "Test complete!"
echo "==================================================================="
