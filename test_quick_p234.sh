#!/bin/bash
MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8096
LOG="test_p234_$(date +%H%M%S).log"

echo "Testing Phase 2+3+4 integration on port $PORT..."

cleanup() { pkill -f "splash serve.*$PORT" 2>/dev/null || true; }
trap cleanup EXIT

# Test: Combined Phase 3+4 flags
echo "Starting server with Phase 3+4 flags..."
./splash serve --model "$MODEL" --kv-format int4 --vision-text-only \
  --kv-swap-enable --kv-swap-hot-tier 180000 --kv-swap-batch-size 32 \
  --enable-weight-prefetch --prefetch-layers 4 \
  --port "$PORT" --host 127.0.0.1 > "$LOG" 2>&1 &
PID=$!

sleep 50

if kill -0 $PID 2>/dev/null; then
  echo "✅ Server running with Phase 3+4 flags"
  echo "Log: $LOG"
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true
  exit 0
else
  echo "❌ Server failed"
  cat "$LOG" | tail -30
  exit 1
fi
