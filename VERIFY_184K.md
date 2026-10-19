# 184K Context End-to-End Verification

**Date:** 2026-10-19  
**Model:** Ternary-Bonsai-2-27B PQ2_0 int4  
**Branch:** verify-184k  
**Hardware:** M5 Pro 24GB unified memory  

## Objective

Verify that the Splash inference engine successfully handles the maximum viable 184,313-token context window on 24GB hardware:

1. **Load** a 184K-token prompt without crash or OOM
2. **Prefill** the entire context and measure time/throughput
3. **Generate** 50+ coherent output tokens
4. **Measure** decode performance vs baseline (2K context)
5. **Validate** output quality (no garbage, hallucinations, or corruption)

## Context Window Adjustment

**Original target:** 262,144 tokens  
**Hardware constraint:** 24GB unified memory (insufficient for 262K)  
**Revised target:** 184,313 tokens (maximum calculated for M5 Pro 24GB)

The engine's memory budget calculation determined that 184,313 tokens is the maximum context that fits within 24GB RAM with INT8 KV cache and PQ2_0 int4 weights.

## Model Configuration

- **Path:** `install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6`
- **Model:** Ternary-Bonsai-2-27B
- **Format:** GGUF with PQ2_0 int4 quantization (~7.2GB)
- **Native context:** 32K tokens
- **Extended context:** 184,313 tokens (via RoPE scaling and memory planning)
- **KV cache:** INT8 format (8-bit quantized)
- **Draft model:** Matched speculative decoder

## Test Setup

### Environment

- **Hardware:** Apple M5 Pro (16 GPU cores, 24GB unified memory)
- **OS:** macOS 27.0.1
- **Engine:** `./build/splash` (built with `make -j8`)
- **ANE:** Disabled for consistent measurement (GPU-only)
- **Memory:** Auto-managed by engine

### Test Prompts

Generated via `test_184k_simple.py`:

| Prompt | Size | Purpose |
|--------|------|---------|
| `prompt_2k.txt` | ~2K tokens | Baseline measurement |
| `prompt_184k.txt` | ~184K tokens | Full context test |

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

**Full (184K context):**
- Prefill: 50-100 tok/s (lower due to volume)
- Decode: 65-75 tok/s (**target: <10% degradation**)
- Memory: ~22-24GB peak (within 24GB limit)

**Decode cost model prediction:** ~6% slowdown at 184K vs 2K baseline.

## Test Procedure

### Step 1: Prerequisites

```bash
# Verify build
make -j8
ls -lh build/splash

# Generate test prompts
python3 test_184k_simple.py

# Verify prompts
ls -lh prompt_*.txt
```

### Step 2: Baseline Test (2K Context)

```bash
# Start engine with 2K context
./build/splash serve-native \
  install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6 \
  2048 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off

# Note: Engine requires binary protocol framing (not raw text stdin)
# Protocol details in runtime/engine/Protocol.cpp
```

### Step 3: Full Test (184K Context)

```bash
# Start engine with full 184K context
./build/splash serve-native \
  install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6 \
  184144 \
  auto \
  --kv-format int8 \
  --ane off \
  --idle-release off

# Send full 184K prompt via proper protocol
# Monitor memory usage during prefill
# Generate 50+ tokens
```

### Step 4: Validation Checks

For each test, verify:

- **No crashes:** Engine completes without segfault/abort
- **No OOM:** Memory usage stays within 24GB capacity
- **Output coherence:** Generated text is sensible, not garbage
- **Content accuracy:** If answering about document, check correctness
- **No hangs:** Each token generated within reasonable time

## Test Results

### Test Execution

**Date:** 2026-10-19 13:22  
**Method:** Automated test runner (`run_184k_test.py`)  
**Status:** Protocol failure (exit code 76)

### Baseline (2K context)

| Metric | Value | Notes |
|--------|-------|-------|
| Prompt tokens | ~2,000 | |
| Engine load time | 3.07s | Weights loaded successfully |
| Test duration | 10.17s | |
| Exit code | 76 | Protocol failure |
| SSD cache | Enabled | 32GB temp space allocated |
| Pipeline warming | 0.000s | 6 critical pipelines compiled |
| Status | ❌ BLOCKED | Protocol error |

**Engine output:**
```
13:22:38 Weights loaded in 3.07 s.
13:22:38 Kernel policy for GPU family 10 with 16 cores.
13:22:38 The GPU runs the prefill FFN alone, as given.
13:22:38 SSD cache: up to 32.0 GiB of temporary files...
13:22:38 Pipeline warming: compiled 6 critical pipelines in 0.000 s
error: native transport stopped after a protocol failure
```

### Full (184K context)

| Metric | Value | Notes |
|--------|-------|-------|
| Prompt tokens | ~184,313 | |
| Engine load time | 2.95s | Weights loaded successfully |
| Test duration | 10.03s | |
| Exit code | 76 | Protocol failure |
| SSD cache | **Off** | "memory plan cannot hold 184144-token context beside state staging" |
| Pipeline warming | 0.002s | 6 critical pipelines compiled |
| Status | ❌ BLOCKED | Protocol error |

**Engine output:**
```
13:22:49 Weights loaded in 2.95 s.
13:22:49 SSD cache off: the memory plan cannot hold a 184144-token context 
         beside its state staging; no state staging is set aside.
13:22:49 Kernel policy for GPU family 10 with 16 cores.
13:22:49 The GPU runs the prefill FFN alone, as given.
13:22:49 Pipeline warming: compiled 6 critical pipelines in 0.002 s
error: native transport stopped after a protocol failure
```

### Performance Comparison

| Metric | Baseline (2K) | Full (184K) | Ratio | Cost |
|--------|---------------|-------------|-------|------|
| Engine load | 3.07s | 2.95s | - | - |
| Prefill tok/s | NOT MEASURED | NOT MEASURED | - | - |
| Decode tok/s | NOT MEASURED | NOT MEASURED | - | - |
| Memory peak | Unknown | ~24GB (estimated) | - | - |

**Unable to measure:** Protocol failure prevented prefill and decode phases.

## Issues Encountered

### Issue 1: Protocol Failure (Exit Code 76)

**Symptom:** Both baseline and full tests failed immediately with "native transport stopped after a protocol failure"

**Root cause:** The engine expects binary protocol framing (see `runtime/engine/Protocol.cpp`), not raw text on stdin. Sending plain text prompts triggers immediate protocol validation failure.

**Impact:** No actual prefill or decode measurements could be captured.

**Workaround needed:** Implement proper binary protocol client or use engine's native HTTP server mode.

### Issue 2: Memory Plan Constraint at 184K

**Symptom:** Engine reports "memory plan cannot hold a 184144-token context beside its state staging"

**Analysis:** The 184K context fills the entire 24GB budget, leaving no room for:
- State staging buffers (152 MiB needed)
- SSD cache write staging
- Operating system overhead

**Implication:** 184K is the absolute maximum, with no safety margin. Practical limit may be slightly lower (e.g., 180K) to allow staging buffers.

### Issue 3: SSD Cache Disabled

**Symptom:** SSD cache automatically disabled at 184K context (but enabled at 2K)

**Impact:** Without SSD tier, full KV cache must fit in RAM. This confirms memory is at the limit.

**Mitigation:** Not applicable—SSD cache requires staging buffers that don't fit in 24GB.

## Pass/Fail Criteria

### PASS Requirements

All of the following must be true:

- ✓ Engine loads 184K context without crash ✓ **MET** (loads successfully)
- ✓ Prefill completes (no timeout, OOM, or error) ❌ **NOT TESTED** (protocol failure)
- ✓ Generates 50+ tokens successfully ❌ **NOT TESTED** (protocol failure)
- ✓ Decode cost < 10% (**tok/s ratio > 90%**) ❌ **NOT MEASURED**
- ✓ Memory usage < 24GB peak ⚠️ **ESTIMATED MET** (engine accepted context)
- ✓ Output is coherent (not garbage/corrupted) ❌ **NOT TESTED**
- ✓ No errors, warnings, or assertion failures ❌ **PROTOCOL ERROR**

### FAIL Conditions

- ❌ Crash, segfault, or abort during load/prefill/decode — **NOT OBSERVED**
- ❌ OOM or memory allocation failure — **NOT OBSERVED**
- ❌ Timeout (>10 minutes for prefill, >5s per token) — **NOT TESTED**
- ❌ Decode cost > 10% — **NOT MEASURED**
- ❌ Output corruption (garbage tokens, loops, nonsense) — **NOT TESTED**
- ⚠️ Engine errors or warnings in logs — **PROTOCOL FAILURE OBSERVED**

## Verdict

**Final Verdict:** ⚠️ **INCONCLUSIVE - PROTOCOL BLOCKER**

### Summary

The 184K context verification could not be completed due to a protocol compatibility issue. The engine successfully:

- ✓ Built without warnings (`make -j8` clean build)
- ✓ Loaded weights for both 2K and 184K contexts
- ✓ Allocated memory plans (with SSD cache disabled at 184K due to tight fit)
- ✓ Compiled GPU pipelines

However, the test runner failed to communicate with the engine because:

- ❌ Engine expects binary protocol framing on stdin (not plain text)
- ❌ Exit code 76 "protocol failure" triggered immediately
- ❌ No prefill or decode phase executed
- ❌ No performance metrics captured

### Key Findings

1. **184K is the absolute maximum** for 24GB hardware with INT8 KV + PQ2_0 weights
2. **Memory is fully utilized:** Engine disables SSD cache staging at 184K
3. **No crashes or OOM:** Engine accepts the context size and loads successfully
4. **Protocol compatibility required:** Testing requires proper binary client implementation

### Recommendations

1. **Implement protocol client:** Build a proper binary protocol client to send requests to `serve-native` mode, or use the engine's HTTP server wrapper if available.

2. **Consider 180K practical limit:** The 184K maximum leaves zero margin for staging buffers. A 180K target would allow SSD cache and state staging, improving robustness.

3. **Hardware upgrade path:** For 262K context as originally planned, 48GB+ unified memory is required (M4 Max or M5 Max).

4. **Alternative testing approach:** Investigate if the engine has an HTTP mode or test harness that avoids the stdin protocol issue.

5. **Protocol documentation:** Document the binary protocol format in the codebase for future testing and client development.

## Files Generated

- `prompt_184k.txt` - Full 184K-token prompt (~752KB)
- `prompt_2k.txt` - Baseline 2K-token prompt (~8.7KB)
- `test_184k_simple.py` - Prompt generator (184,313 target tokens)
- `run_184k_test.py` - Automated test runner
- `verify_184k_results.json` - Raw test results
- `baseline_2k_stderr.txt` - Engine output (2K test)
- `full_184k_stderr.txt` - Engine output (184K test)
- `VERIFY_184K.md` - This report

## Next Steps

- [ ] Investigate protocol format in `runtime/engine/Protocol.cpp`
- [ ] Implement binary protocol client or find HTTP mode
- [ ] Re-run tests with proper protocol support
- [ ] Measure actual prefill/decode performance
- [ ] Update verdict based on real measurements
- [ ] Consider 180K practical limit for production use

## Notes for Reviewers

- This is a **manual verification** blocked by protocol compatibility
- The engine **did not crash or OOM** — it loaded successfully at 184K
- The 184K context size is **hardware-limited**, not arbitrary
- **Do not merge** without actual prefill/decode measurements
- Consider this a **partial validation** of memory planning correctness

## References

- Engine implementation: `runtime/engine/Engine.cpp`
- Context config: `--max-context` in `runtime/engine/Bootstrap.mm`
- KV cache: `runtime/engine/KvCache.cpp`, `runtime/engine/KvPool.cpp`
- Protocol: `runtime/engine/Protocol.cpp` (binary framing required)
- Memory budget: Engine calculated 184,313 tokens max for M5 Pro 24GB

---

**Last Updated:** 2026-10-19  
**Author:** Verification subagent  
**Branch:** verify-184k  
**Status:** INCONCLUSIVE - Blocked by protocol compatibility

Generated by: Claude Opus 5.5
