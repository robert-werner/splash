#!/bin/bash
# Быстрое тестирование оптимизаций Splash
set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
RESULTS_DIR="quick_test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Быстрое тестирование оптимизаций"
echo "Модель: $MODEL"
echo "=========================================="

test_config() {
    local test_name=$1
    local port=$2
    shift 2
    local extra_args="$@"
    
    echo ""
    echo "=== Тест: $test_name ==="
    echo "Флаги: $extra_args"
    
    local server_log="$RESULTS_DIR/${test_name}_${TIMESTAMP}.log"
    
    echo "Запуск сервера..."
    ./splash serve \
        --model "$MODEL" \
        --kv-format int4 \
        --vision-text-only \
        --port "$port" \
        --host 127.0.0.1 \
        $extra_args > "$server_log" 2>&1 &
    
    local server_pid=$!
    echo "Server PID: $server_pid"
    
    sleep 20
    
    if ! kill -0 $server_pid 2>/dev/null; then
        echo "❌ Сервер упал!"
        tail -30 "$server_log"
        return 1
    fi
    
    echo "✅ Сервер запущен"
    
    # Один тестовый запрос
    local prompt="Write a Python function to calculate Fibonacci numbers"
    echo "Выполнение запроса: $prompt"
    
    local start_time=$(date +%s.%N)
    
    local response=$(curl -s -X POST "http://127.0.0.1:$port/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d "{
            \"model\": \"$MODEL\",
            \"messages\": [{\"role\": \"user\", \"content\": \"$prompt\"}],
            \"max_tokens\": 150,
            \"temperature\": 0.7
        }" 2>&1)
    
    local end_time=$(date +%s.%N)
    local duration=$(echo "$end_time - $start_time" | bc)
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    if echo "$response" | grep -q "\"content\""; then
        local tokens=$(echo "$response" | jq -r '.usage.completion_tokens // 0' 2>/dev/null || echo "0")
        local tok_per_sec=$(echo "scale=2; $tokens / $duration" | bc)
        
        echo "✅ Успех: $tokens токенов за ${duration}s ($tok_per_sec tok/s)"
        echo "$test_name,$tokens,$duration,$tok_per_sec" >> "$RESULTS_DIR/summary_${TIMESTAMP}.csv"
        
        # Метрики из server log
        echo ""
        echo "📋 Метрики (последние 30 строк):"
        tail -30 "$server_log" | grep -E "(tok/s|TTFT|Weights loaded|GPU|ANE)" || echo "  (метрики не найдены)"
    else
        echo "❌ Ошибка: $response"
        return 1
    fi
    
    return 0
}

echo "test_name,tokens,time_s,tok_per_sec" > "$RESULTS_DIR/summary_${TIMESTAMP}.csv"

# Baseline
test_config "baseline" 8090

# Phase 3: KV Swap
test_config "phase3_kv_swap" 8091 \
    --kv-swap-enable on \
    --kv-swap-ssd-path ./test_swap \
    --kv-swap-threshold 0.90

echo ""
echo "=========================================="
echo "✅ Тестирование завершено!"
echo "=========================================="
echo ""
cat "$RESULTS_DIR/summary_${TIMESTAMP}.csv" | column -t -s ','
