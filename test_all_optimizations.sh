#!/bin/bash
# Комплексное тестирование всех оптимизаций Splash Inference Engine
# Дата: 2026-10-18
# Модель: prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0

set -e

MODEL="prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT_BASE=8090
RESULTS_DIR="comprehensive_test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Комплексное тестирование оптимизаций"
echo "Модель: $MODEL"
echo "Дата: $(date)"
echo "=========================================="

# Функция для тестирования конфигурации
test_config() {
    local test_name=$1
    local port=$2
    shift 2
    local extra_args="$@"
    
    echo ""
    echo "=== Тест: $test_name ==="
    echo "Порт: $port"
    echo "Дополнительные флаги: $extra_args"
    
    local log_file="$RESULTS_DIR/${test_name}_${TIMESTAMP}.log"
    local server_log="$RESULTS_DIR/${test_name}_server_${TIMESTAMP}.log"
    
    # Запуск сервера
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
    
    # Ожидание старта сервера
    echo "Ожидание готовности сервера..."
    sleep 15
    
    # Проверка что сервер жив
    if ! kill -0 $server_pid 2>/dev/null; then
        echo "❌ Сервер упал при старте!"
        tail -50 "$server_log"
        return 1
    fi
    
    echo "✅ Сервер запущен"
    
    # Тестовые промпты
    local prompts=(
        "Write a Python function to calculate Fibonacci numbers"
        "Explain quantum entanglement in simple terms"
        "What is the capital of France?"
    )
    
    local total_tokens=0
    local total_time=0
    local request_count=0
    
    echo "Выполнение тестовых запросов..."
    
    for prompt in "${prompts[@]}"; do
        echo "  Промпт: $prompt"
        
        local start_time=$(date +%s.%N)
        
        local response=$(curl -s -X POST "http://127.0.0.1:$port/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{
                \"model\": \"$MODEL\",
                \"messages\": [{\"role\": \"user\", \"content\": \"$prompt\"}],
                \"max_tokens\": 100,
                \"temperature\": 0.7
            }" 2>&1)
        
        local end_time=$(date +%s.%N)
        local duration=$(echo "$end_time - $start_time" | bc)
        
        # Проверка ответа
        if echo "$response" | grep -q "\"content\""; then
            local tokens=$(echo "$response" | jq -r '.usage.completion_tokens // 0' 2>/dev/null || echo "0")
            local tok_per_sec=$(echo "scale=2; $tokens / $duration" | bc)
            
            echo "    ✅ Успех: $tokens токенов за ${duration}s ($tok_per_sec tok/s)"
            
            total_tokens=$((total_tokens + tokens))
            total_time=$(echo "$total_time + $duration" | bc)
            request_count=$((request_count + 1))
        else
            echo "    ❌ Ошибка: $response"
        fi
        
        sleep 2
    done
    
    # Остановка сервера
    echo "Остановка сервера..."
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    # Результаты
    if [ $request_count -gt 0 ]; then
        local avg_tok_per_sec=$(echo "scale=2; $total_tokens / $total_time" | bc)
        echo ""
        echo "📊 Результаты $test_name:"
        echo "  Запросов: $request_count"
        echo "  Токенов: $total_tokens"
        echo "  Время: ${total_time}s"
        echo "  Средняя скорость: $avg_tok_per_sec tok/s"
        
        # Сохранение в CSV
        echo "$test_name,$request_count,$total_tokens,$total_time,$avg_tok_per_sec" >> "$RESULTS_DIR/summary_${TIMESTAMP}.csv"
        
        # Извлечение метрик из server log
        echo ""
        echo "📋 Метрики сервера (последние 20 строк):"
        tail -20 "$server_log" | grep -E "(tok/s|TTFT|Weights loaded)" || echo "  (метрики не найдены)"
    else
        echo "❌ Все запросы провалились!"
        return 1
    fi
    
    echo ""
    echo "Лог сервера сохранён: $server_log"
    
    return 0
}

# Создание CSV header
echo "test_name,requests,tokens,time_s,avg_tok_per_sec" > "$RESULTS_DIR/summary_${TIMESTAMP}.csv"

# Тест 1: Baseline (без дополнительных оптимизаций)
test_config "baseline" 8090

# Тест 2: Phase 1 - Pipeline Warming (уже включён по умолчанию)
# Warming автоматически активен, проверим TTFT

# Тест 3: Phase 3 - Async KV Swap
test_config "phase3_kv_swap" 8091 \
    --kv-swap-enable on \
    --kv-swap-ssd-path ./test_swap \
    --kv-swap-threshold 0.90

# Тест 4: Phase 2 - ANE Hybrid Quantization (если поддерживается)
# Примечание: полная интеграция не завершена, но CLI парсинг работает
test_config "phase2_ane_hybrid" 8092 \
    --ane-layers "0,2,4,8,10,14"

# Тест 5: Все оптимизации вместе
test_config "all_optimizations" 8093 \
    --kv-swap-enable on \
    --kv-swap-ssd-path ./test_swap_all \
    --kv-swap-threshold 0.90 \
    --ane-layers "0,2,4,8,10,14"

echo ""
echo "=========================================="
echo "✅ Тестирование завершено!"
echo "=========================================="
echo ""
echo "Результаты сохранены в: $RESULTS_DIR/"
echo "Сводка: $RESULTS_DIR/summary_${TIMESTAMP}.csv"
echo ""
echo "📊 Итоговая таблица:"
cat "$RESULTS_DIR/summary_${TIMESTAMP}.csv" | column -t -s ','

echo ""
echo "Для детального анализа смотрите логи:"
ls -lh "$RESULTS_DIR/"*"${TIMESTAMP}.log"
