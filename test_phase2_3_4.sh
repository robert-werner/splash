#!/bin/bash
set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
BUILD="./build/splash"
TIMEOUT=180

echo "=== Phase 2+3+4 Integration Test ==="
echo "Model: $MODEL"
echo ""

# Test 1: Baseline (current)
echo "[1/4] Baseline (no optimizations)..."
timeout $TIMEOUT $BUILD serve-native "$MODEL" 200000 auto \
  2>&1 | tee /tmp/baseline.log | grep -E "(context|decode|ANE)" || true

# Test 2: Phase 3 (KV Swap)
echo ""
echo "[2/4] Phase 3: KV Swap enabled..."
timeout $TIMEOUT $BUILD serve-native "$MODEL" 200000 auto \
  --kv-swap-enable \
  --kv-swap-hot-tier 180000 \
  --kv-swap-threshold 0.90 \
  2>&1 | tee /tmp/phase3.log | grep -E "(context|swap|evict)" || true

# Test 3: Phase 4 (Weight Prefetch)
echo ""
echo "[3/4] Phase 4: Weight Prefetch enabled..."
timeout $TIMEOUT $BUILD serve-native "$MODEL" 200000 auto \
  --enable-weight-prefetch \
  --prefetch-layers 4 \
  2>&1 | tee /tmp/phase4.log | grep -E "(prefetch|latency)" || true

# Test 4: Combined (Phase 3+4)
echo ""
echo "[4/4] Combined: KV Swap + Weight Prefetch..."
timeout $TIMEOUT $BUILD serve-native "$MODEL" 200000 auto \
  --kv-swap-enable \
  --kv-swap-hot-tier 180000 \
  --enable-weight-prefetch \
  --prefetch-layers 4 \
  2>&1 | tee /tmp/combined.log | grep -E "(context|decode|prefetch|swap)" || true

echo ""
echo "=== Test Complete ==="
echo "Logs saved:"
echo "  /tmp/baseline.log"
echo "  /tmp/phase3.log"
echo "  /tmp/phase4.log"
echo "  /tmp/combined.log"
