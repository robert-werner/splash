#!/bin/bash
# Комплексное тестирование Phase 1-4 на prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0
# Generated: 2026-10-18

set -euo pipefail

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT=8081
LOG_DIR="test_logs_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

echo "=== Комплексное тестирование Phase 1-4 ==="
echo "Модель: $MODEL"
echo "Логи: $LOG_DIR"
echo ""

# Убить предыдущие процессы
pkill -9 splash 2>/dev/null || true
sleep 2

# Тестовый промпт
PROMPT="Write a detailed technical explanation of how neural network attention mechanisms work, including mathematical formulas and implementation details."

# Функция для отправки запроса
send_request() {
    local name=$1
    local extra_args=$2
    local log_file="$LOG_DIR/${name}.log"
    
    echo ">>> Тест: $name"
    echo ">>> Флаги: $extra_args"
    
    # Запуск сервера через launcher
    ./splash serve \
        --model "$MODEL" \
        --port $PORT \
        --max-batch-size 4 \
        --working-set 19G \
        --vision-text-only \
        $extra_args \
        > "$log_file" 2>&1 &
    
    local server_pid=$!
    echo ">>> Server PID: $server_pid"
    
    # Ждём готовности сервера
    sleep 30
    
    # Проверка что сервер жив
    if ! kill -0 $server_pid 2>/dev/null; then
        echo "❌ Сервер не запустился"
        cat "$log_file"
        return 1
    fi
    
    # Отправка запроса
    local start_time=$(date +%s.%N)
    
    curl -s http://localhost:$PORT/v1/chat/completions \
        -H "Content-Type: application/json" \
        -d "{
            \"model\": \"$MODEL\",
            \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\"}],
            \"max_tokens\": 500,
            \"temperature\": 0.7
        }" > "$LOG_DIR/${name}_response.json" 2>&1
    
    local end_time=$(date +%s.%N)
    local duration=$(echo "$end_time - $start_time" | bc)
    
    echo ">>> Время выполнения: ${duration}s"
    
    # Извлечение метрик из лога
    local ttft=$(grep -oP '(?<=First token: )\d+\.\d+' "$log_file" | tail -1 || echo "N/A")
    local tps=$(grep -oP '(?<=tokens/sec: )\d+\.\d+' "$log_file" | tail -1 || echo "N/A")
    local total_tokens=$(grep -oP '(?<=Generated: )\d+' "$log_file" | tail -1 || echo "N/A")
    
    echo ">>> TTFT: ${ttft}ms"
    echo ">>> Tokens/sec: $tps"
    echo ">>> Total tokens: $total_tokens"
    echo ""
    
    # Убить сервер
    kill -9 $server_pid 2>/dev/null || true
    sleep 3
    
    # Сохранить метрики
    echo "$name,$ttft,$tps,$total_tokens,$duration" >> "$LOG_DIR/metrics.csv"
}

# CSV заголовок
echo "test,ttft_ms,tokens_per_sec,total_tokens,duration_s" > "$LOG_DIR/metrics.csv"

# 1. Baseline (без оптимизаций)
send_request "baseline" ""

# 2. Phase 1: Metal shader warming
send_request "phase1_warming" ""

# 3. Phase 3: AsyncKvSwap (hot tier 180K)
send_request "phase3_kvswap" "--kv-swap-enable --kv-swap-hot-tier 180000"

# 4. Phase 4: Weight prefetch
send_request "phase4_prefetch" "--enable-weight-prefetch on --prefetch-layers 4"

# 5. Combined Phase 3 + Phase 4
send_request "phase3_4_combined" "--kv-swap-enable --kv-swap-hot-tier 180000 --enable-weight-prefetch on --prefetch-layers 4"

# 6. Phase 2: ANE hybrid (stub, no actual ANE yet)
send_request "phase2_ane_stub" "--ane-layers 0,2,4,8,10,14"

# 7. All phases combined
send_request "all_phases" "--kv-swap-enable --kv-swap-hot-tier 180000 --enable-weight-prefetch on --prefetch-layers 4 --ane-layers 0,2,4,8,10,14"

echo ""
echo "=== Результаты тестирования ==="
cat "$LOG_DIR/metrics.csv"
echo ""
echo "Полные логи: $LOG_DIR/"

# Финальный анализ
echo ""
echo "=== Анализ прироста производительности ==="
python3 -c "
import csv
import sys

with open('$LOG_DIR/metrics.csv') as f:
    reader = csv.DictReader(f)
    data = list(reader)

baseline = next(d for d in data if d['test'] == 'baseline')
baseline_tps = float(baseline['tokens_per_sec']) if baseline['tokens_per_sec'] != 'N/A' else 0
baseline_ttft = float(baseline['ttft_ms']) if baseline['ttft_ms'] != 'N/A' else 0

print('Тест | TTFT (ms) | Δ TTFT | Tokens/sec | Δ TPS')
print('-' * 60)

for row in data:
    name = row['test']
    ttft = float(row['ttft_ms']) if row['ttft_ms'] != 'N/A' else 0
    tps = float(row['tokens_per_sec']) if row['tokens_per_sec'] != 'N/A' else 0
    
    ttft_delta = ((ttft - baseline_ttft) / baseline_ttft * 100) if baseline_ttft > 0 else 0
    tps_delta = ((tps - baseline_tps) / baseline_tps * 100) if baseline_tps > 0 else 0
    
    print(f'{name:20s} | {ttft:8.1f} | {ttft_delta:+6.1f}% | {tps:10.1f} | {tps_delta:+6.1f}%')
" 2>/dev/null || echo "Python анализ недоступен"

echo ""
echo "✅ Тестирование завершено"
