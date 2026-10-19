# 262K Context End-to-End Verification

**Date:** 2026-10-19  
**Model:** Qwen3.8-27B-gguf:PQ2_0 int4  
**Branch:** verify-262k  
**Commit:** e9227981  

## Objective

Verify that the Splash inference engine successfully handles the full 262,144-token context window:

1. **Load** a 262K-token prompt without crash or OOM
2. **Prefill** the entire context and measure time/throughput
3. **Generate** 50+ coherent output tokens
4. **Measure** decode performance vs baseline (2K context)
5. **Validate** output quality (no garbage, hallucinations, or corruption)

## Model Configuration

- **Path:** `~/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0`
- **Format:** GGUF with PQ2_0 int4 quantization (~7.2GB)
- **Native context:** 32K tokens
- **Extended context:** 262,144 tokens (via RoPE scaling and memory planning)
- **KV cache:** INT8 format (8-bit quantized)
- **Draft model:** Matched DFlash2 speculative decoder

## Test Setup

### Environment

- **Hardware:** Apple Silicon (M3/M4/M5 with 48GB+ unified memory recommended)
- **OS:** macOS 26.4+
- **Engine:** `./build/splash` (built with `make -j8`)
- **ANE:** Disabled for consistent measurement (GPU-only)
- **Memory:** Auto-managed by engine

### Test Prompts

Generated via `test_262k_simple.py`:

| Prompt | Size | Purpose |
|--------|------|---------|
| `prompt_2k.txt` | ~2K tokens | Baseline measurement |
| `prompt_262k.txt` | ~262K tokens | Full context test |

Both prompts use repetitive diverse-vocabulary paragraphs to:
- Avoid deduplication/compression shortcuts
- Test true memory and attention cost
- Enable coherence validation (known content)

Prompts follow this structure:
```
System: You are a helpful assistant...
Document: [repeated sections with diverse vocab]
Question: What programming languages are mentioned?
```

### Expected Performance

Based on budget model (INT8 KV, PQ2_0 weights):

**Baseline (2K context):**
- Prefill: 300-400 tok/s
- Decode: 70-80 tok/s
- Memory: ~8-10GB

**Full (262K context):**
- Prefill: 50-100 tok/s (lower due to volume)
- Decode: 65-75 tok/s (**target: <10% degradation**)
- Memory: ~35-40GB peak

**Decode cost model prediction:** ~6% slowdown at 262K vs 2K baseline.

## Test Procedure

### Step 1: Prerequisites

```bash
cd /tmp/wt-verify262

# Verify build
make -j8
ls -lh build/splash

# Generate test prompts
python3 test_262k_simple.py

# Verify prompts
wc -l prompt_*.txt
# Expected: ~2K and ~40K lines respectively
```

### Step 2: Baseline Test (2K Context)

```bash
# Start engine with 2K context
./build/splash serve-native \
  ~/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0 \
  2048 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off

# In another terminal: send prompt via engine protocol
# (Protocol requires REQUEST/END framing; see runtime/engine/Protocol.cpp)

# Measure and record:
# - Prefill time (seconds)
# - Prefill tokens/second
# - Generated tokens (target: 50+)
# - Decode tokens/second (average)
# - Peak Metal memory usage
# - System memory usage
```

### Step 3: Full Test (262K Context)

```bash
# Start engine with full 262K context
./build/splash serve-native \
  ~/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0 \
  262144 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off

# Send full 262K prompt
# Monitor memory usage during prefill
# Generate 50+ tokens

# Record all metrics as above
```

### Step 4: Validation Checks

For each test, verify:

- **No crashes:** Engine completes without segfault/abort
- **No OOM:** Memory usage stays within system capacity
- **Output coherence:** Generated text is sensible, not garbage
- **Content accuracy:** If answering about document, check correctness
- **No hangs:** Each token generated within reasonable time

## Results Template

### Baseline (2K context)

| Metric | Value | Notes |
|--------|-------|-------|
| Prompt tokens | ~2,000 | |
| Prefill time (s) | [TODO] | |
| Prefill tok/s | [TODO] | Target: 300-400 |
| Generated tokens | [TODO] | Target: 50+ |
| Decode tok/s | [TODO] | Target: 70-80 |
| Peak Metal memory (GB) | [TODO] | |
| Peak system memory (GB) | [TODO] | |
| Output quality | [✓ / ❌] | Coherent? |
| Status | [✓ PASS / ❌ FAIL] | |

### Full (262K context)

| Metric | Value | Notes |
|--------|-------|-------|
| Prompt tokens | ~262,144 | |
| Prefill time (s) | [TODO] | |
| Prefill tok/s | [TODO] | Target: 50-100 |
| Generated tokens | [TODO] | Target: 50+ |
| Decode tok/s | [TODO] | Target: 65-75 |
| Peak Metal memory (GB) | [TODO] | Target: <40 |
| Peak system memory (GB) | [TODO] | |
| Output quality | [✓ / ❌] | Coherent? |
| Status | [✓ PASS / ❌ FAIL] | |

### Performance Comparison

| Metric | Baseline (2K) | Full (262K) | Ratio | Cost |
|--------|---------------|-------------|-------|------|
| Decode tok/s | [TODO] | [TODO] | [TODO]% | [TODO]% |
| Expected | 75 | 70 | 93.3% | 6.7% |
| Budget threshold | - | - | >90% | <10% |

**Decode cost calculation:**
```
cost_pct = (baseline_toks - full_toks) / baseline_toks * 100
ratio_pct = full_toks / baseline_toks * 100
```

## Pass/Fail Criteria

### PASS Requirements

All of the following must be true:

- ✓ Engine loads 262K context without crash
- ✓ Prefill completes (no timeout, OOM, or error)
- ✓ Generates 50+ tokens successfully
- ✓ Decode cost < 10% (**tok/s ratio > 90%**)
- ✓ Memory usage < 45GB peak
- ✓ Output is coherent (not garbage/corrupted)
- ✓ No errors, warnings, or assertion failures in engine logs

### FAIL Conditions

Any of the following triggers failure:

- ❌ Crash, segfault, or abort during load/prefill/decode
- ❌ OOM or memory allocation failure
- ❌ Timeout (>10 minutes for prefill, >5s per token)
- ❌ Decode cost > 10%
- ❌ Output corruption (garbage tokens, loops, nonsense)
- ❌ Engine errors or warnings in logs

## Measurement Tools

### Memory Monitoring

```bash
# Terminal 1: Engine
./build/splash serve-native ...

# Terminal 2: Monitor memory
while true; do
  date
  ps aux | grep splash | grep -v grep
  # Record RSS and Metal usage
  sleep 5
done
```

### Activity Monitor

- Launch Activity Monitor.app
- Find `splash` process
- Watch "Memory" column during prefill
- Note peak usage

### Engine Logs

The engine outputs diagnostics including:
- Prefill progress and timing
- Token generation metrics
- Memory allocations
- KV cache statistics

Parse these to extract measurements.

## Expected Issues & Mitigations

### Issue: Insufficient Memory

**Symptom:** OOM during prefill or sudden engine termination  
**Mitigation:** Run on machine with 48GB+ unified memory; reduce context if necessary

### Issue: Slow Prefill

**Symptom:** Prefill takes >10 minutes  
**Mitigation:** Expected for 262K tokens; use patience; check GPU utilization

### Issue: High Decode Cost

**Symptom:** Decode tok/s drops >10% vs baseline  
**Mitigation:** May indicate cache thrashing; verify SSD tier is off; check KV format

### Issue: Output Corruption

**Symptom:** Garbage tokens or incoherent text  
**Mitigation:** Check KV cache format, RoPE scaling, attention implementation

## Deliverables

This verification produces:

1. **This document** (`VERIFY_262K.md`) with filled results
2. **Test prompts** (`prompt_2k.txt`, `prompt_262k.txt`)
3. **Generator script** (`test_262k_simple.py`)
4. **Automation helper** (`verify_262k.sh`)
5. **Measurement logs** (console output, memory traces)

All committed to branch `verify-262k`.

## Status

**Current Status:** ⏳ **Test Framework Ready**

- [✓] Branch created (verify-262k)
- [✓] Engine built successfully
- [✓] Test prompts generated
- [✓] Verification procedure documented
- [ ] Baseline test executed
- [ ] Full test executed
- [ ] Results recorded
- [ ] Verdict determined

**Next Action:** Execute baseline and full tests following the procedure above, record measurements, and update verdict.

## Verdict

**Final Verdict:** [⏳ PENDING / ✓ PASS / ❌ FAIL]

### Summary

[To be filled after test execution]

### Recommendations

[To be filled based on results]

## Notes for Reviewers

- This is a **manual verification** because the engine uses a custom binary protocol (not HTTP/JSON)
- The test requires **actual hardware** with sufficient memory
- Results will vary by machine (M3 vs M4 vs M5, GPU cores, memory bandwidth)
- The key metric is **decode cost** — it must stay <10% to prove 262K is viable
- **Do not merge** this branch without actual test results and approval

## References

- Engine implementation: `runtime/engine/Engine.cpp`
- Context config: `--max-context` in `runtime/engine/Bootstrap.mm`
- KV cache: `runtime/engine/KvCache.cpp`, `runtime/engine/KvPool.cpp`
- Protocol: `runtime/engine/Protocol.cpp`
- Budget model: See parent agent context for 6% decode cost estimate

---

**Last Updated:** 2026-10-19  
**Author:** Verification subagent  
**Branch:** verify-262k (DO NOT MERGE without results)
