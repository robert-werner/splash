# TTFT Measurement Status Report

## Задача
Измерить TTFT для 100K token prefill с различными размерами чанков для определения оптимальной стратегии чанкинга.

## Test Matrix
1. Chunk size 8K tokens → measure TTFT
2. Chunk size 16K tokens → measure TTFT
3. Chunk size 32K tokens → measure TTFT
4. No chunking (100K single prefill) → measure TTFT

## Текущий Статус

### Подход 1: Множественные engine instances (НЕУДАЧА)
**Скрипт**: `measure_chunking_ttft.py`
**Результат**: Все 4 теста провалились из-за недостатка памяти

**Проблема**: 
- Engine требует ~11.5 GiB памяти для инициализации
- Система имеет критический memory pressure
- Каждый тест пытался запустить отдельный instance engine
- Результат: SIGKILL или timeout при запуске

**Лог**: `chunking_ttft_log.txt`
**Результаты**: `chunking_ttft_results.json` (все failed)

Пример ошибки:
```
error: runtime bootstrap failed [memory_audit]: not enough free memory to start: 
2029 MiB reclaimable, 2048 MiB protected for macOS, system pressure warning
```

### Подход 2: Переиспользование одного engine instance (В ПРОЦЕССЕ)
**Скрипт**: `measure_chunking_ttft_reuse.py`
**Статус**: Запущен в background с session_id=`proc_0059253fae17`

**Оптимизации**:
1. Один engine instance для всех 4 тестов
2. Увеличен timeout для ReadyEvent: 30s → 120s
3. Клиент ждёт, пока engine получит достаточно памяти

**Передано родительскому агенту**: Процесс будет завершен асинхронно, родитель получит уведомление

## Ожидаемые Результаты

На основе baseline (179K tokens = 894s TTFT):

### Для 100K tokens:
- **Expected baseline** (monolithic): ~497s (100K / 179K * 894s)
- **Expected с chunking**:
  - 8K chunks: ~19.5s TTFT (первый чанк @ 420 tok/s)
  - 16K chunks: ~38.1s TTFT
  - 32K chunks: ~76.2s TTFT
  - Monolithic: ~497s TTFT

### Метрики для сравнения:
- TTFT (Time To First Token)
- Throughput (tokens/second)
- Improvement vs monolithic (%)
- Improvement vs expected baseline (%)

## Файлы

### Скрипты:
- `measure_chunking_ttft.py` - первая версия (множественные instances)
- `measure_chunking_ttft_reuse.py` - оптимизированная версия (один instance)

### Результаты:
- `chunking_ttft_log.txt` - лог первого запуска (failed)
- `chunking_ttft_results.json` - результаты первого запуска (все failed)
- `chunking_ttft_reuse_log2.txt` - лог текущего запуска (в процессе)
- `chunking_ttft_results_reuse.json` - будет создан после завершения

### Зависимости:
- `binary_protocol_client.py` - клиент бинарного протокола (timeout увеличен до 120s)

## Изменения в Коде

### binary_protocol_client.py
Увеличен timeout для `_wait_for_ready()`:
```python
def _wait_for_ready(self, timeout: float = 120.0) -> ReadyEvent:
    # было: 30.0
```

Причина: Engine требует больше времени для получения достаточной памяти при system pressure.

## Следующие Шаги

1. Дождаться завершения процесса `proc_0059253fae17`
2. Проанализировать результаты из `chunking_ttft_results_reuse.json`
3. Если успешно: определить оптимальный размер чанка
4. Если неуспешно: рассмотреть альтернативные подходы:
   - Уменьшить target tokens (например, 50K вместо 100K)
   - Использовать меньшую модель для тестирования
   - Освободить больше системной памяти
   - Тестировать на другой системе с большим количеством RAM

## Техническая Информация

**Система**: macOS 27.0.1, M5 Pro, 24GB RAM
**Модель**: 27B (baseline assembly)
**Context size**: 180000 tokens
**KV format**: int8
**ANE**: off
**Memory**: auto

**Memory Requirements**:
- Fixed runtime: ~11 GiB
- Elastic state/KV budget: ~6.2 GiB
- Minimum required: ~11.5 GiB
- Total physical: 24 GiB
- Current pressure: Critical/Warning
