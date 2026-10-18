#!/bin/bash
set -euo pipefail

echo "=== Phase 2 Hybrid GGUF Quick Test ==="
echo "Model: Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf"
echo "ANE layers: 0,2,4,8,10,14"
echo ""

# Start server
echo "[1/3] Starting server with ANE layers..."
./build/splash serve-native install/models/Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf \
  --ane-layers "0,2,4,8,10,14" \
  --port 8097 \
  --max-batch-size 4 \
  --ane-telemetry on \
  > hybrid_test.log 2>&1 &
SERVER_PID=$!

# Wait for server ready
echo "Waiting for server to start..."
for i in {1..30}; do
  if curl -s http://127.0.0.1:8097/v1/models >/dev/null 2>&1; then
    echo "✅ Server ready (PID $SERVER_PID)"
    break
  fi
  sleep 1
done

# Test single decode
echo ""
echo "[2/3] Testing single decode throughput..."
curl -s http://127.0.0.1:8097/v1/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "any",
    "prompt": "Once upon a time",
    "max_tokens": 100,
    "temperature": 0.7
  }' > /dev/null 2>&1 &
DECODE_PID=$!

# Wait 10 seconds then extract throughput
sleep 10

# Extract metrics from logs
echo ""
echo "[3/3] Extracting metrics from logs..."
echo ""
grep -i "pipeline warming" hybrid_test.log || echo "⚠️  No pipeline warming found"
grep -i "ane" hybrid_test.log | head -5 || echo "⚠️  No ANE telemetry found"
grep -i "tok/s\|tokens/s" hybrid_test.log | tail -3 || echo "⚠️  No throughput metrics found"

# Clean up
kill $SERVER_PID $DECODE_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Test complete. Full logs in hybrid_test.log ==="
