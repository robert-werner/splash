# Quick Start: 262K Context Verification

## Prerequisites

1. **Model:** Download if needed
   ```bash
   splash serve --model unsloth/Qwen3.8-27B-GGUF:PQ2_0
   # Wait for download, then Ctrl+C
   ```

2. **Build:** Ensure engine is compiled
   ```bash
   cd /tmp/wt-verify262
   make -j8
   ```

3. **Prompts:** Already generated (prompt_2k.txt, prompt_262k.txt)

## Run Tests

### Baseline (2K context)

```bash
./build/splash serve-native \
  ~/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0 \
  2048 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off
```

Record metrics: prefill tok/s, decode tok/s, memory usage.

### Full (262K context)

```bash
./build/splash serve-native \
  ~/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0 \
  262144 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off
```

Record same metrics. Compare decode tok/s.

## Pass Criteria

- ✓ No crashes or OOM
- ✓ Generates 50+ tokens
- ✓ Decode cost < 10% (decode tok/s ratio > 90%)

## Results

Fill in VERIFY_262K.md with measurements and update verdict.

## Files

- **VERIFY_262K.md** - Full test procedure and results template
- **test_262k_simple.py** - Prompt generator
- **verify_262k.sh** - Automation helper
- **prompt_262k.txt** - 262K test prompt (1.1MB)
- **prompt_2k.txt** - Baseline prompt (8.7KB)

## Status

Branch `verify-262k` ready. Execute tests, record results, commit updates.

**DO NOT MERGE** without actual measurements and approval.
