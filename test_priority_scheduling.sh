#!/bin/bash
# Test script for context-aware priority scheduling
#
# Compares latency metrics between:
# 1. Baseline FIFO scheduling (--priority-scheduling off)
# 2. Context-aware priority scheduling (--priority-scheduling on)

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081
DURATION=120
INTERACTIVE_RATE=4.0
BATCH_RATE=1.0

echo "=========================================="
echo "Priority Scheduling Latency Comparison"
echo "=========================================="
echo "Model: $MODEL"
echo "Duration: ${DURATION}s per configuration"
echo "Interactive rate: ${INTERACTIVE_RATE} req/s (512 tokens)"
echo "Batch rate: ${BATCH_RATE} req/s (12K tokens)"
echo ""

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    lsof -ti:$PORT | xargs -r kill -9 2>/dev/null || true
    sleep 2
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

# Function to wait for server readiness
wait_for_server() {
    echo "Waiting for server to be ready..."
    for i in {1..30}; do
        if curl -s "http://localhost:$PORT/v1/models" > /dev/null 2>&1; then
            echo "Server is ready!"
            return 0
        fi
        sleep 1
    done
    echo "Server failed to start!"
    return 1
}

# Test 1: Baseline FIFO scheduling
echo "=========================================="
echo "TEST 1: Baseline FIFO Scheduling"
echo "=========================================="
cleanup

echo "Starting server with --priority-scheduling off..."
./splash serve --model "$MODEL" --port $PORT \
    --priority-scheduling off \
    > /tmp/baseline_server.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

if ! wait_for_server; then
    echo "Baseline test failed: server did not start"
    cat /tmp/baseline_server.log
    exit 1
fi

echo "Running baseline benchmark..."
python3 measure_mixed_latency.py \
    --port $PORT \
    --duration $DURATION \
    --interactive-rate $INTERACTIVE_RATE \
    --batch-rate $BATCH_RATE \
    > /tmp/baseline_results.txt 2>&1

echo "Baseline benchmark complete!"
cat /tmp/baseline_results.txt

# Save baseline results
cp /tmp/mixed_latency_results.json /tmp/baseline_latency.json

cleanup

# Test 2: Context-aware priority scheduling
echo ""
echo "=========================================="
echo "TEST 2: Context-Aware Priority Scheduling"
echo "=========================================="

echo "Starting server with --priority-scheduling on --preempt-threshold 4096..."
./splash serve --model "$MODEL" --port $PORT \
    --priority-scheduling on \
    --preempt-threshold 4096 \
    > /tmp/priority_server.log 2>&1 &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"

if ! wait_for_server; then
    echo "Priority test failed: server did not start"
    cat /tmp/priority_server.log
    exit 1
fi

echo "Running priority scheduling benchmark..."
python3 measure_mixed_latency.py \
    --port $PORT \
    --duration $DURATION \
    --interactive-rate $INTERACTIVE_RATE \
    --batch-rate $BATCH_RATE \
    > /tmp/priority_results.txt 2>&1

echo "Priority scheduling benchmark complete!"
cat /tmp/priority_results.txt

# Save priority results
cp /tmp/mixed_latency_results.json /tmp/priority_latency.json

cleanup

# Compare results
echo ""
echo "=========================================="
echo "COMPARISON SUMMARY"
echo "=========================================="

echo ""
echo "BASELINE (FIFO):"
grep -A 20 "INTERACTIVE REQUESTS" /tmp/baseline_results.txt | head -8

echo ""
echo "PRIORITY SCHEDULING:"
grep -A 20 "INTERACTIVE REQUESTS" /tmp/priority_results.txt | head -8

echo ""
echo "Full results:"
echo "  Baseline: /tmp/baseline_results.txt"
echo "  Priority: /tmp/priority_results.txt"
echo "  Raw data: /tmp/baseline_latency.json, /tmp/priority_latency.json"
echo ""
echo "=========================================="
