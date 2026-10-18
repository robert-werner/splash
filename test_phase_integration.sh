#!/bin/bash
set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
BUILD="./build/splash"

echo "=== Phase 2+3+4 Integration Test ==="
echo "Model: $MODEL"
echo ""

# Test 1: Baseline
echo "[1/4] Baseline (no optimizations)..."
$BUILD serve-native "$MODEL" 200000 auto 2>&1 | head -100 > /tmp/baseline.log &
PID=$!
sleep 10
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
grep -E "(context|decode|memory)" /tmp/baseline.log | head -20

# Test 2: Phase 3 KV Swap
echo ""
echo "[2/4] Phase 3: KV Swap..."
$BUILD serve-native "$MODEL" 200000 auto \
  --kv-swap-enable \
  --kv-swap-hot-tier 180000 \
  2>&1 | head -100 > /tmp/phase3.log &
PID=$!
sleep 10
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
grep -E "(swap|evict|hot.*tier)" /tmp/phase3.log | head -20

# Test 3: Phase 4 Weight Prefetch
echo ""
echo "[3/4] Phase 4: Weight Prefetch..."
$BUILD serve-native "$MODEL" 200000 auto \
  --enable-weight-prefetch \
  --prefetch-layers 4 \
  2>&1 | head -100 > /tmp/phase4.log &
PID=$!
sleep 10
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
grep -E "(prefetch|weight)" /tmp/phase4.log | head -20

echo ""
echo "=== CLI Flags Test ==="
$BUILD --help 2>&1 | grep -E "serve-native" | head -3
echo ""
echo "Full logs: /tmp/{baseline,phase3,phase4}.log"
