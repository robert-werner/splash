> SUPERSEDED by STATUS.md — its roadmap assumes ANE hybrid gives +30%, treats PTQ1_0 as a decode win, and uses 62.9 tok/s as the baseline; ANE is unreachable, PTQ1_0 changes only storage and load time, and the baseline is not comparable

# Глубокий анализ оптимизации Splash для M5 Pro 24 ГБ / 1 ТБ SSD

**Дата анализа:** 2026-10-17  
**Цель:** Максимизация context window, токенов/сек и утилизация ANE/GPU/SSD для моделей Qwen

---

## Текущее состояние (baseline)

### Достигнутые показатели
- **Context:** 262,144 токена (100% от max модели PQ2_0)
- **Decode:** 62.9 tok/s (single request)
- **Throughput:** 143.5 tok/s (batch 4)
- **GPU load:** 99.4% (bottleneck: weight bandwidth 111 GB/s)
- **Memory:** 14.8 ГБ total (10.6 fixed + 4.2 KV int4)
- **SSD cache:** 78 ГБ persistent

### Реализованные оптимизации
✅ INT4 KV (коммиты `82d8185`, `abe4a8a`, `4e1c022`) — ×1.97 memory saving  
✅ `--working-set` (коммит `cd5dc2b`) — разблокировал полный context  
✅ `--recalibrate-ane` (коммит `7fa979b`) — сброс ANE calibration  
✅ `--lazy-draft-loading` (коммит `59de303`) — отложенная загрузка draft (~1.18 ГБ)

---

## Анализ бутылочных горлышек

### 1. Weight bandwidth — главный bottleneck (99.4% GPU)
```
Decode cycle: 76 ms
Weight traffic: 7.93 ГБ (target 6.75 + draft 1.18)
Bandwidth: ~111 GB/s @ 2-bit quant
Dequant ops: ~34B weights/cycle = ~450B weights/sec
```

**Диагноз:** GPU полностью занят чтением и деквантованием весов. Арифметика attention бесплатна (batch 1→4: cycle 76→76 мс, throughput ×4.4).

**Следствия:**
- ✅ Batch scaling почти бесплатен → `--max-batch-size 4` уже optimal
- ✅ Speculation даёт ×2.5 (acceptance 18–27% не критичен — ряды бесплатны)
- ❌ Single-thread decode speed не поднять без разгрузки FFN на ANE

---

### 2. ANE простаивает (0% utilization)

**Причина:** PQ2_0 использует **input rotation** (GGUF_ROTATION_BLOCK 1024, Адамар transform), требуемую для 2-bit точности. ANE требует `!rotation` → **взаимоисключающие требования**.

**Альтернатива:** Q4_K_M (rotation=false) → ANE доступен, но:
- Веса: 6.75 ГБ → 15.3 ГБ (×2.27)
- Context: 262K → ~200K (−24%)
- Decode: 62.9 → 24–28 tok/s (−60%)
- Prefill: +30–50% via ANE FFN split

**Вывод:** Для максимального context (262K) ANE недоступен; Q4_K_M подходит для RAG/short generation.

---

### 3. Memory pressure: фиксировано 10.6 ГБ из 24 ГБ

```
Breakdown (PQ2_0, vision off):
  Target weights (27B PQ2_0):  6.75 ГБ
  Draft weights (DFlash):       1.18 ГБ ← можно отложить
  Vision tower:                 0.87 ГБ ← уже выключен
  Prefill+decode scratch:       1.04 ГБ
  Staging + reserves:           0.90 ГБ
  ────────────────────────────────────
  Fixed:                       10.74 ГБ
  
  KV cache budget:             13.26 ГБ (для 262K @ int4)
  Реально используется:         4.20 ГБ (262K @ int4)
  
  Остаток под macOS:            5.45 ГБ
```

**Проблема:** Draft model (1.18 ГБ) загружается всегда, даже если speculation не используется.

**Решение:** ✅ `--lazy-draft-loading` уже реализован — освобождает 1.18 ГБ = +38K токенов context (но уже упёрлись в 262K модели).

---

### 4. SSD cache: 78 ГБ персистентный, но не расширяет context

**Текущая работа:**
- Persistent KV prefixes → снижение TTFT на повторяющихся промптах
- State checkpoints → переживают перезапуск
- Quota: 1/4 свободного места (322 ГБ free → 78 ГБ cache)

**Ограничение:** SSD-tier НЕ расширяет resident context — страницы должны быть в RAM для attention.

**Возможность:** Реализовать **SSD-backed KV extension** для ultra-long context (1M+ tokens).

---

## Нереализованные возможности оптимизации

### А. Context expansion (уже исчерпан для PQ2_0)

| Метод | Выигрыш | Статус |
|-------|---------|--------|
| INT4 KV | 180K → 262K | ✅ Реализовано |
| Vision off | +28K | ✅ Автоматически не нужен с int4 |
| --working-set 19G | +180K → 262K | ✅ Реализовано |
| Draft model off | +38K | ⏳ Lazy loading реализован, но 262K уже достигнут |
| INT2 KV | +131K (теоретически) | ❌ Нет int2b_format в Metal |

**Вывод:** Context для PQ2_0 уже упёрся в максимум модели (262K). Дальнейшее расширение требует:
1. Другую модель с большим max_position_embeddings
2. SSD-backed extension (см. раздел Б4)

---

### Б. Скорость decode (текущий bottleneck: weight bandwidth)

#### Б1. ANE acceleration (недоступен для PQ2_0)

**Концепция:** Разгрузить prefill FFN с GPU на ANE (~38% compute).

**Блокер:** PQ2_0 rotation=true несовместим с ANE.

**Альтернативные модели:**
- ✅ Q4_K_M: ANE доступен, +30–50% prefill, −60% decode
- ❌ Нужна PQ2_0-like модель БЕЗ rotation — не существует

**Предложение:** Экспериментальная квантизация PQ2_0 с rotation-free вариантом (требует переквантизации модели).

---

#### Б2. Weight prefetching & pipelining

**Текущая проблема:** Decode cycle = 76 мс, из них ~76 мс на weight transfer (111 GB/s @ 7.93 ГБ).

**Идея:** Prefetch следующего слоя в background во время compute текущего.

**Реализация:**
```cpp
// Псевдокод
DecodeStep(layer_idx):
  1. Wait for layer[idx] weights (blocking)
  2. Compute FFN + attention
  3. Async prefetch layer[idx+1] weights → staging buffer
  
// Overlapping:
  GPU compute layer[i]  ████████████████
  Transfer layer[i+1]         ██████████████
```

**Выигрыш:** Теоретически до 20–30% при полном перекрытии, практически ~10–15% (compute/transfer overlap).

**Сложность:** Требует:
- Double-buffering для weight staging
- Async command encoding в Metal
- Coordination между transfer и compute queues

**Приоритет:** 🟡 Средний — требует глубоких изменений в `Runtime.cpp`, но даёт измеримый выигрыш.

---

#### Б3. Kernel fusion: attention + FFN

**Текущее состояние:** RMS norm + FFN уже сплавлены (`norm_rms_table16_decode`), RoPE инлайн в QKV.

**Дополнительный fusion:**
```
Current:  [Attention] → [LayerNorm] → [FFN]
Proposed: [Attention+LayerNorm+FFN_gate] (single kernel)
```

**Выигрыш:** Убрать 2 memory round-trips (attention output + norm output) → экономия ~5–10% bandwidth.

**Сложность:** Высокая — требует переписывания attention kernel с inline norm/FFN.

**Приоритет:** 🟡 Средний — измеримый выигрыш, но сложная реализация.

---

#### Б4. Speculation tuning

**Текущее:** Draft model 1.18 ГБ, acceptance 18–27%, throughput ×2.5.

**Идея 1: Adaptive draft depth**
```cpp
if (acceptance_rate < 15%)
  draft_tokens = 2;  // shallow draft
else if (acceptance_rate > 30%)
  draft_tokens = 8;  // deep draft
else
  draft_tokens = 4;  // default
```

**Выигрыш:** ×2.5 → ×2.8 при высоком acceptance.

**Идея 2: Draft model quantization**
```
Current: DFlash 1.18 ГБ
INT4 draft: ~600 МБ → освобождает 600 МБ = +19K context
```

**Приоритет:** 🟢 Высокий — малая сложность, измеримый выигрыш.

---

### В. Batch optimization

#### В1. Dynamic batching (частично реализовано)

**Текущее:** `DynamicBatching.cpp` существует, но логика неполная.

**Предложение:**
```cpp
// Adaptive batch size based on GPU utilization
if (gpu_util < 95%)
  batch_size = min(batch_size + 1, max_batch_size);
else if (gpu_util > 99% && latency_p99 > target)
  batch_size = max(1, batch_size - 1);
```

**Выигрыш:** Автоматическая балансировка throughput vs latency.

**Приоритет:** 🟢 Высокий — инфраструктура уже есть, нужна доработка.

---

#### В2. Context-aware scheduling

**Идея:** Приоритизировать запросы по длине context:
- Short context (< 10K): низкая задержка → high priority
- Long context (> 100K): высокая TTFT → batch агрессивнее

**Выигрыш:** Снижение p99 latency для коротких запросов при batch inference.

**Приоритет:** 🟡 Средний — требует изменений в `Scheduler.cpp`.

---

### Г. SSD-backed KV extension

#### Г1. Tiered KV cache architecture

**Концепция:**
```
Tier 0 (RAM hot):   0–200K tokens @ int4    (4.2 ГБ)
Tier 1 (SSD cold): 200K–1M tokens @ int4   (13.5 ГБ)
                                            ────────
                                            17.7 ГБ = 1M tokens
```

**Механизм:**
1. Hot tier: resident in RAM, standard paged attention
2. Cold tier: evicted to SSD via persistent cache
3. On access: async page fault → load from SSD (~50–200 ms)
4. Prefetch: sequential scan heuristics for context window

**Выигрыш:** Context 262K → **1M+ tokens** (ограничен SSD cache 78 ГБ).

**Trade-off:**
- Latency: +50–200 мс на первое обращение к cold page
- Throughput: −10% при частых random accesses
- Sequential scan (summarization): минимальный overhead при prefetch

**Реализация:**
```cpp
// runtime/ops/PagedKv.hpp
class TieredKvStorage : public ExtentStorage {
  RamTier hot_;
  SsdTier cold_;
  LruEvictionPolicy eviction_;
  
  void accessPage(uint32_t page_id) {
    if (!hot_.contains(page_id)) {
      // Page fault: load from SSD
      cold_.asyncLoad(page_id, [this](Page p) {
        hot_.promote(p);
      });
    }
  }
};
```

**Приоритет:** 🟢 Высокий — уникальная возможность для ultra-long context (book-scale RAG).

**Сложность:** Высокая — требует:
- Async page loading infrastructure
- LRU eviction policy
- Prefetch heuristics
- Integration с `PagedAttention.cpp`

**Измеримый выигрыш:**
- RAG на книге (500K tokens): 262K → 500K context
- Legal document analysis (1M tokens): impossible → possible

---

#### Г2. Compression для cold tier

**Идея:** Холодные KV в SSD хранить в int2 или ZSTD-compressed.

```
Hot (RAM):  int4 @ 16,896 bytes/token
Cold (SSD): int2 @  8,448 bytes/token (lossy)
         or ZSTD @ ~6,000 bytes/token (lossless, slower)
```

**Выигрыш:** SSD 78 ГБ → 2M+ tokens (int2) или 1.5M tokens (ZSTD).

**Trade-off:**
- INT2: качество падает на старых токенах (приемлемо для summarization)
- ZSTD: decompression latency +20–50 мс

**Приоритет:** 🟡 Средний — расширение Г1, не standalone feature.

---

### Д. ANE workarounds

#### Д1. Hybrid FFN split (GPU + ANE для unrotated слоёв)

**Идея:** Если модель имеет rotated и unrotated слои (маловероятно), разделить FFN:
- Unrotated layers → ANE
- Rotated layers → GPU

**Проблема:** PQ2_0 имеет rotation на ВСЕХ слоях → не применимо.

**Приоритет:** ❌ Не реализуемо для текущих Qwen моделей.

---

#### Д2. Экспериментальная rotation-free PQ2 квантизация

**Идея:** Переквантизовать Qwen3.8-27B в PQ2 БЕЗ rotation, принять небольшую потерю точности.

**Процесс:**
1. Взять fp16 базу Qwen3.8-27B
2. Квантизовать в 2-bit без Hadamard rotation
3. Измерить perplexity loss
4. Если acceptable → получить 6.75 ГБ веса + ANE support

**Выигрыш:** ANE prefill +30–50%, context 262K сохраняется, decode 62.9 tok/s.

**Риск:** Quality degradation неизвестна — требуется экспериментальная проверка.

**Приоритет:** 🟡 Средний — high-risk/high-reward, требует квантизационной экспертизы.

---

### Е. Metal GPU optimization

#### Е1. FP16 accumulation в matmul

**Текущее:** `matmul2d` использует fp32 accumulation.

**Идея:** Переключить на fp16 accumulation для QK/PV matmul (attention kernels).

```metal
// Current: float accumulator
float sum = 0.0;
for (int k = 0; k < K; k++)
  sum += A[k] * B[k];

// Proposed: half accumulator
half sum = 0.0h;
for (int k = 0; k < K; k++)
  sum += half(A[k]) * half(B[k]);
```

**Выигрыш:** ~20–30% bandwidth saving, ~10–15% speedup на attention.

**Trade-off:** Небольшая потеря точности (обычно неразличима на практике).

**Приоритет:** 🟡 Средний — требует тестирования quality impact.

---

#### Е2. Threadgroup memory optimization

**Текущее:** Attention kernels используют threadgroup memory для shared K/V tiles.

**Идея:** Увеличить tile size для лучшей локальности:
```
Current: 64×64 tiles
Proposed: 128×128 tiles (если влезает в threadgroup memory)
```

**Выигрыш:** Меньше global memory reads → +5–10% attention throughput.

**Приоритет:** 🟢 Высокий — low-risk optimization, быстрая реализация.

---

### Ж. Qwen-specific optimizations

#### Ж1. Sliding window attention (для Qwen >= 32B)

**Идея:** Qwen 32B+ использует sliding window attention (window 4K–8K).

**Оптимизация:**
```cpp
// Prefill: full attention на window, sparse на старых токенах
if (token_position > window_size)
  attention_mask = sliding_window_mask(window_size);
```

**Выигрыш:** Prefill TTFT снижается с O(n²) → O(n×window).

**Применимость:** Только для Qwen >= 32B, которые не влезают в 24 ГБ.

**Приоритет:** ❌ Не применимо к текущим целевым моделям.

---

#### Ж2. MoE routing optimization (для Qwen3.6-35B MoE)

**Проблема:** 35B MoE требует 22.6 ГБ fixed → не влезает в 24 ГБ.

**Идея:** Offload inactive experts to SSD, load on-demand.

**Выигрыш:** 35B MoE становится возможным на 24 ГБ.

**Сложность:** Очень высокая — требует:
- Expert routing prediction
- Async expert loading
- Weight cache management

**Приоритет:** 🟡 Средний — unlocks 35B, но высокая сложность.

---

## Приоритизированный план реализации

### 🔥 High Priority (максимальный ROI, низкая сложность)

#### 1. Speculation tuning (B4)
**Выигрыш:** ×2.5 → ×2.8 throughput  
**Сложность:** Низкая (адаптация существующего кода)  
**Оценка:** 2–3 дня

**Реализация:**
- Adaptive draft depth на основе acceptance rate
- Draft model quantization (INT8 → INT4)

---

#### 2. Dynamic batching доработка (В1)
**Выигрыш:** Автоматическая балансировка latency/throughput  
**Сложность:** Низкая (инфраструктура уже есть)  
**Оценка:** 1–2 дня

**Реализация:**
- Доработать `DynamicBatching.cpp` с GPU utilization feedback
- Добавить p99 latency tracking

---

#### 3. Threadgroup memory optimization (Е2)
**Выигрыш:** +5–10% attention throughput  
**Сложность:** Низкая (изменения в Metal kernels)  
**Оценка:** 1–2 дня

**Реализация:**
- Увеличить tile size в `paged_attention_tile.h`
- Профилирование и tuning

---

### 🟡 Medium Priority (высокий ROI, средняя сложность)

#### 4. SSD-backed KV extension (Г1) — ⭐ KILLER FEATURE
**Выигрыш:** Context 262K → **1M+ tokens**  
**Сложность:** Высокая (новая архитектура)  
**Оценка:** 1–2 недели

**Реализация:**
- `TieredKvStorage` с hot/cold tiers
- Async page loading infrastructure
- LRU eviction policy
- Prefetch heuristics

**Обоснование:** Уникальная возможность для ultra-long context, которую не предлагают конкуренты.

---

#### 5. Weight prefetching & pipelining (Б2)
**Выигрыш:** +10–15% decode speed  
**Сложность:** Средняя (async coordination)  
**Оценка:** 3–5 дней

**Реализация:**
- Double-buffering для weight staging
- Async Metal command encoding
- Transfer/compute queue coordination

---

#### 6. Context-aware scheduling (В2)
**Выигрыш:** Снижение p99 latency для коротких запросов  
**Сложность:** Средняя (изменения в Scheduler)  
**Оценка:** 2–3 дня

---

### 🔵 Low Priority (исследовательские/рискованные)

#### 7. Rotation-free PQ2 квантизация (Д2)
**Выигрыш:** ANE support + 262K context + 62.9 tok/s  
**Риск:** Неизвестная потеря качества  
**Оценка:** 1–2 недели (эксперимент)

---

#### 8. Kernel fusion: attention + FFN (Б3)
**Выигрыш:** +5–10% bandwidth savings  
**Сложность:** Очень высокая  
**Оценка:** 2–3 недели

---

#### 9. MoE expert offloading (Ж2)
**Выигрыш:** 35B MoE на 24 ГБ  
**Сложность:** Очень высокая  
**Оценка:** 3–4 недели

---

## Рекомендованная стратегия на ближайшие 2 недели

### Неделя 1: Quick Wins
1. ✅ **День 1-2:** Speculation tuning (Б4)
2. ✅ **День 3:** Dynamic batching (В1)
3. ✅ **День 4-5:** Threadgroup memory optimization (Е2)

**Ожидаемый результат:**
- Throughput: 143.5 → ~160 tok/s (batch 4)
- Single decode: 62.9 → 65–68 tok/s
- Latency: улучшение p99 на 10–15%

---

### Неделя 2: Killer Feature
4. ⭐ **День 6-14:** SSD-backed KV extension (Г1)

**Ожидаемый результат:**
- Context: 262K → **1M+ tokens**
- Use case: book-scale RAG, legal documents, ultra-long conversations

---

## Измеримые целевые KPI

### Текущий baseline (PQ2_0, M5 Pro 24 ГБ)
```
Context:           262,144 tokens
Decode (single):   62.9 tok/s
Throughput (b=4):  143.5 tok/s
GPU utilization:   99.4%
Memory:            14.8 ГБ (10.6 fixed + 4.2 KV)
SSD cache:         78 ГБ
```

### Целевые показатели после оптимизаций (2 недели)
```
Context:           1,000,000 tokens  (+282%)
Decode (single):   65-68 tok/s       (+3-8%)
Throughput (b=4):  160-165 tok/s     (+11-15%)
GPU utilization:   99.5%             (стабильно)
Memory hot:        14.8 ГБ           (без изменений)
Memory cold (SSD): ~13 ГБ            (для 738K cold tokens)
```

### Долгосрочные цели (1-2 месяца)
```
Context:           2,000,000 tokens  (+663%)
Decode (single):   70-75 tok/s       (+11-19%)
Throughput (b=4):  180-190 tok/s     (+25-32%)
ANE utilization:   30-40% (с rotation-free quant)
```

---

## Выводы

### Текущее состояние: Near-Optimal для PQ2_0
- Context уже **максимальный** (262K = 100% модели)
- GPU **полностью загружен** (99.4%)
- Batch scaling **оптимален** (×4.4 throughput)
- SSD cache **работает** (78 ГБ persistent)

### Главные ограничения:
1. **Weight bandwidth** — единственный реальный bottleneck
2. **ANE недоступен** для PQ2_0 (rotation блокирует)
3. **Resident context** ограничен RAM (SSD не расширяет)

### Три ключевых направления:
1. ⭐ **SSD-backed KV** — killer feature (262K → 1M+ context)
2. 🚀 **Speculation tuning** — быстрый выигрыш (+10-15% throughput)
3. 🔬 **Rotation-free PQ2** — исследование (ANE + max context)

### Рекомендация:
**Начать с SSD-backed KV extension** — это уникальная возможность, которая даст качественный скачок для ultra-long context use cases (book-scale RAG, legal docs, multi-session conversations).

---

**Автор:** Claude Opus 5.5  
**Железо:** M5 Pro 24 ГБ / 1 ТБ SSD  
**Baseline модель:** Qwen3.8-27B (PQ2_0)
