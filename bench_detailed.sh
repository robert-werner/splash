#!/bin/bash
set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8084
RUNS=10

lsof -ti:$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
sleep 1

echo "Starting server..."
./splash serve --model "$MODEL" --port $PORT --kv-format int4 > /tmp/bench_detail.log 2>&1 &
SERVER_PID=$!

for i in {1..60}; do
  if curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
    echo "Server ready"
    break
  fi
  sleep 1
done

if ! curl -s http://localhost:$PORT/v1/models >/dev/null 2>&1; then
  echo "Server failed"
  kill $SERVER_PID 2>/dev/null || true
  exit 1
fi

# Warm-up
for i in {1..3}; do
  PROMPT=$(python3 -c "print('Warm ' * 512)")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  sleep 0.5
done

echo ""
echo "=== 2K tokens x $RUNS runs ==="
for run in $(seq 1 $RUNS); do
  # Generate unique random numbers → ~2K tokens after BOS
  PROMPT=$(python3 -c "import random; random.seed($run); print(' '.join(str(random.randint(10000, 99999)) for _ in range(300)))")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  echo -n "."
  sleep 0.3
done
echo ""

echo ""
echo "=== 4K tokens x $RUNS runs ==="
for run in $(seq 1 $RUNS); do
  # Generate unique random numbers → ~4K tokens after BOS
  PROMPT=$(python3 -c "import random; random.seed($run + 100); print(' '.join(str(random.randint(10000, 99999)) for _ in range(600)))")
  curl -s http://localhost:$PORT/v1/completions \
    -H "Content-Type: application/json" \
    -d "{\"model\": \"$MODEL\", \"prompt\": \"$PROMPT\", \"max_tokens\": 1, \"temperature\": 0.7}" > /dev/null
  echo -n "."
  sleep 0.3
done
echo ""

sleep 2
kill $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Results ==="

# Parse 2K results (~1800 tokens, cached=0)
grep "Done.*cached 0.*TTFT" /tmp/bench_detail.log | grep "input 1," | \
  grep -oE "TTFT [0-9.]+s" | awk '{print $2}' | sed 's/s$//' | python3 -c "
import sys
times = [float(line.strip()) for line in sys.stdin]
if times:
    times_sorted = sorted(times)
    print(f'2K cold: min={min(times):.2f}s median={times_sorted[len(times)//2]:.2f}s max={max(times):.2f}s avg={sum(times)/len(times):.2f}s n={len(times)}')
else:
    print('2K: no data')
"

# Parse 4K results (~3600 tokens, cached=0)
grep "Done.*cached 0.*TTFT" /tmp/bench_detail.log | grep "input 3," | \
  grep -oE "TTFT [0-9.]+s" | awk '{print $2}' | sed 's/s$//' | python3 -c "
import sys
times = [float(line.strip()) for line in sys.stdin]
if times:
    times_sorted = sorted(times)
    print(f'4K cold: min={min(times):.2f}s median={times_sorted[len(times)//2]:.2f}s max={max(times):.2f}s avg={sum(times)/len(times):.2f}s n={len(times)}')
else:
    print('4K: no data')
"
