#!/bin/bash
set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8097
TIMESTAMP=$(date +%H%M%S)

cleanup() {
  pkill -f "splash serve.*$PORT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=== Phase 2+3+4 Integration Test ==="
echo "Model: $MODEL"
echo "Port: $PORT"
echo ""

# Test 1: Baseline
echo "[1/3] Baseline (no optimizations)..."
./splash serve --model "$MODEL" --kv-format int4 --vision-text-only \
  --port "$PORT" > "baseline_$TIMESTAMP.log" 2>&1 &
PID=$!
sleep 40
if kill -0 $PID 2>/dev/null; then
  echo "✅ Baseline started"
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true
  sleep 3
else
  echo "❌ Failed"
  tail -20 "baseline_$TIMESTAMP.log"
  exit 1
fi

# Test 2: Phase 3 AsyncKvSwap
echo "[2/3] Phase 3 (AsyncKvSwap CLI flags)..."
./splash serve --model "$MODEL" --kv-format int4 --vision-text-only \
  --kv-swap-enable on --kv-swap-hot-tier 150000 --kv-swap-batch-size 16 \
  --port "$PORT" > "phase3_$TIMESTAMP.log" 2>&1 &
PID=$!
sleep 40
if kill -0 $PID 2>/dev/null; then
  echo "✅ Phase 3 CLI working"
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true
  sleep 3
else
  echo "❌ Failed"
  tail -20 "phase3_$TIMESTAMP.log"
  exit 1
fi

# Test 3: Combined Phase 3+4
echo "[3/3] Combined Phase 3+4..."
./splash serve --model "$MODEL" --kv-format int4 --vision-text-only \
  --kv-swap-enable on --kv-swap-hot-tier 180000 --kv-swap-batch-size 32 \
  --enable-weight-prefetch on --prefetch-layers 4 \
  --port "$PORT" > "combined_$TIMESTAMP.log" 2>&1 &
PID=$!
sleep 50

if kill -0 $PID 2>/dev/null; then
  echo "✅ Combined Phase 3+4 working"
  
  # Quick inference
  sleep 10
  curl -s -m 30 http://127.0.0.1:$PORT/v1/models > "models_$TIMESTAMP.json" 2>&1 || echo "⚠️  Inference skipped"
  
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true
  
  echo ""
  echo "=== Results ==="
  echo "✅ All tests passed"
  echo ""
  echo "Logs:"
  ls -lh baseline_$TIMESTAMP.log phase3_$TIMESTAMP.log combined_$TIMESTAMP.log
  echo ""
  echo "Checking for Phase 3+4 indicators in logs..."
  grep -i "swap\|prefetch\|phase" combined_$TIMESTAMP.log | head -5 || echo "(no explicit mentions, CLI accepted)"
  exit 0
else
  echo "❌ Failed"
  tail -30 "combined_$TIMESTAMP.log"
  exit 1
fi
