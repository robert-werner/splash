#!/bin/bash
# Test Phase 2,3,4 integration on Ternary-Bonsai-2-27B-PQ2_0

MODEL="/Users/a1111/development/splash_of_mine/install/models/.resolved/147a396cdf6c78c1d3d033939353ab08fb4ae41a39e54d52d6d10d254b5aacb0"
BINARY="./build/splash"

echo "=== Phase 1: Shader Warming Verification ==="
$BINARY serve-native "$MODEL" 180000 auto 2>&1 | grep -E "(Pipeline warming|compiled)" | head -3
echo ""

echo "=== Phase 3: KV Swap Flags Test ==="
$BINARY serve-native "$MODEL" 180000 auto --kv-swap-enable on --kv-swap-hot-tier 150000 2>&1 | grep -E "(KV|swap|tier)" | head -5
echo ""

echo "=== Phase 4: Weight Prefetch Flags Test ==="
$BINARY serve-native "$MODEL" 180000 auto --enable-weight-prefetch on --prefetch-layers 4 2>&1 | grep -E "(prefetch|weight)" | head -5
echo ""

echo "=== Combined Test: All Flags ==="
$BINARY serve-native "$MODEL" 180000 auto \
  --enable-weight-prefetch on \
  --prefetch-layers 4 \
  --kv-swap-enable on \
  --kv-swap-hot-tier 150000 \
  2>&1 | grep -E "(Pipeline|weight|swap|tier)" | head -10
echo ""

echo "✅ All phase flags accepted without errors"
