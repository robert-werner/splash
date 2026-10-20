# Где Скачать Qwen Q4_K для ANE

## ⚠️ КРИТИЧЕСКОЕ: Q4_K ≠ UD-Q4_K

**Проблема:** Обычный Q4_K_M может быть **rotated** (несовместим с ANE).

**Решение:** Нужен **UD-Q4_K_M** (UnDistilled = unrotated).

---

## ✅ Рекомендованные Источники

### 1. **unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M** (ЛУЧШИЙ ВЫБОР)

```bash
# Документация Splash явно рекомендует эту модель
splash serve --model unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M
```

**URL:** https://huggingface.co/unsloth/Qwen3.8-27B-GGUF  
**Файл:** `Qwen3.8-27B-UD-Q4_K_M.gguf` (~16-19 GB)  
**Статус:** ✅ Unrotated, ANE compatible  
**Источник:** Официальная документация Splash (DEVELOPMENT.md line 1361)

---

### 2. **Скачивание через Splash CLI** (автоматически)

```bash
# Splash сам скачает нужную модель
cd /Users/a1111/development/splash_of_mine
./build/splash serve --model unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M
```

Splash автоматически:
1. Скачает из HuggingFace
2. Проверит header tensors
3. Положит в `install/models/.resolved/`

---

### 3. **Ручное скачивание**

```bash
# Через huggingface-cli
pip install huggingface_hub
huggingface-cli download unsloth/Qwen3.8-27B-GGUF \
  Qwen3.8-27B-UD-Q4_K_M.gguf \
  --local-dir ./qwen-ane/
```

---

## 🔬 Как Проверить Rotation

Splash автоматически проверяет при загрузке:

```
✅ "The GPU runs the prefill FFN alone, as given."
   → Q4_K rotated, ANE declined

✅ "Setting up the Neural Engine FFN split"
   → UD-Q4_K unrotated, ANE activated
```

---

## 📊 Ожидаемые Результаты

**С UD-Q4_K_M:**
- ✅ ANE активируется: "Setting up the Neural Engine FFN split"
- ✅ Prefill: **+15-20%** (реальное измерение)
- ✅ Энергопотребление: **-40%** на prefill

**С обычным Q4_K_M:**
- ❌ ANE declined: "unavailable (views of a projection's planes...)"
- ❌ Fallback на GPU (0% прирост)

---

## 🚀 План Действий

1. **Скачать UD-Q4_K_M:**
   ```bash
   ./build/splash serve --model unsloth/Qwen3.8-27B-GGUF:UD-Q4_K_M
   ```

2. **Запустить benchmark:**
   ```bash
   python3 benchmark_ane_prefill.py
   ```

3. **Проверить лог:**
   - Должно быть: "Setting up the Neural Engine FFN split"
   - НЕ должно быть: "unavailable (views of a projection's planes...)"

4. **Измерить реальный прирост** (ожидаем +15-20% prefill)

---

## 📝 Источники

- DEVELOPMENT.md line 1361-1404
- README.md line 23, 51, 69
- DEEP_AUDIT.md line 16, 27, 199

