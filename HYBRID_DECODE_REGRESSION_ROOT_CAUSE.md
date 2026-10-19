# Hybrid GGUF Decode Throughput Regression - Root Cause Analysis

**Generated:** 2026-10-19 17:05:00  
**System:** M5 Pro 24GB, macOS 27.0.1  
**Investigation Status:** Analysis-only (no servers run, memory constraint)

---

## Executive Summary

The hybrid GGUF model (`Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf`, 6.6GB) shows a **−60% decode throughput regression** compared to the baseline PQ2_0 model at large context sizes:

| Context Size | Baseline PQ2_0 | Hybrid Model | Regression |
|--------------|----------------|--------------|------------|
| ~13 tokens   | ~62.9 tok/s    | 47.5 tok/s   | −24.5%     |
| ~14.5K tokens| ~62.9 tok/s    | 25.3 tok/s   | −59.8%     |

**Context sensitivity:** The hybrid model shows a **−46.8%** throughput drop from small to large context (47.5 → 25.3 tok/s), indicating a context-dependent issue.

---

## Investigation Findings

### 1. Configuration Inconsistency

**Critical discovery:** Both `install/models/.resolved/93ac2e4c...` (baseline) and `install/models/hybrid-real` have **identical `model.json` files** that reference the same PQ2_0 GGUF:

```json
"target/Ternary-Bonsai-2-27B-PQ2_0.gguf": {
  "bytes": 7206168928,
  "digest": "3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1",
  "path": "/Users/a1111/.cache/huggingface/hub/models--prism-ml--Ternary-Bonsai-2-27B-gguf/snapshots/.../Ternary-Bonsai-2-27B-PQ2_0.gguf"
}
```

However, `install/models/hybrid-real/` contains a **separate GGUF file**:
- `Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf` (6.6GB)
- This file is **not referenced in model.json**

**Implication:** The server may be loading the wrong GGUF file, or the configuration system is not properly detecting the hybrid model.

### 2. Hybrid Quantization Format

Based on documentation analysis (`STATUS.md`, `PTQ1_0_IMPLEMENTATION_REPORT.md`):

**Format characteristics:**
- **PQ2_0:** 34 bytes/128 weights (2-bit ternary, rotated)
- **PTQ1_0:** 28 bytes/128 weights on disk → **34 bytes/128 weights in runtime** (ternary base-3 codewords, rotated)
- **Hybrid model:** Mix of PTQ1_0 (84 tensors) + PQ2_0 (767 tensors)

**Key insight from STATUS.md:**
> "Both formats therefore have identical *runtime* geometry — plane0 8 bytes per 32-element group (32 B per 128-element block) plus one fp16 scale per block, 34 B per block in memory"

**Expected result:** PTQ1_0 and PQ2_0 should have **identical decode performance** because they expand to the same runtime format.

**Actual result:** The hybrid model shows severe performance degradation, especially at large context.

### 3. Context Sensitivity Analysis

The regression is **context-dependent**:

| Metric | Small Context (13 tok) | Large Context (14.5K tok) | Delta |
|--------|------------------------|---------------------------|-------|
| Hybrid | 47.5 tok/s            | 25.3 tok/s                | −46.8% |

**Baseline expectation:** From `STATUS.md`, decode is weight-bandwidth-bound with flat cycle time:
> "Measured cycle time is **flat at 74.8–76.5 ms for 8, 16 and 32 verify rows**. Quadrupling the rows does not lengthen the cycle."

**Inference:** The hybrid model's context sensitivity indicates a problem unrelated to pure weight bandwidth:
- Memory access pattern inefficiency
- KV cache overhead
- Attention kernel performance degradation
- Potential bug in hybrid quantization code path

### 4. Documented Performance Characteristics

From `STATUS.md` (§2.1 - The reference measurement):

**Baseline PQ2_0 performance:**
- **Marginal decode:** 39.4 tok/s (authoritative, Python server path)
- **Weight bandwidth:** 7.93 GiB/cycle, ~111 GB/s sustained
- **Cycle time:** 74.8–76.5 ms (flat across batch sizes)

**Warning from STATUS.md:**
> "Older figures of **62.9 tok/s**, 107 tok/s, 72.6 tok/s, 84.8 tok/s and 30–36 tok/s that appear in archived reports came from different launch paths (the `serve-native` binary driven directly instead of through the Python server front end), different memory configurations, or single un-repeated runs on a thermally drifting machine."

**Implication:** The 62.9 tok/s baseline cited in `HYBRID_BASELINE_DECODE.md` may be from a different launch path, making direct comparison problematic.

---

## Root Cause Hypotheses

### Hypothesis 1: Configuration Mismatch ⭐ **MOST LIKELY**

**Evidence:**
- `model.json` points to PQ2_0 GGUF for both baseline and hybrid models
- Actual hybrid GGUF (`Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf`) exists but isn't referenced
- Server may be loading wrong model file

**Test:** Verify which GGUF file the server actually loads when serving `install/models/hybrid-real`

**Fix:** Correct `model.json` to reference the actual hybrid GGUF file

### Hypothesis 2: Hybrid Quantization Path Bug

**Evidence:**
- Mixed PTQ1_0/PQ2_0 format requires runtime conversion (PTQ1_0's 28B → 34B expansion)
- Context-dependent performance degradation suggests memory access pattern issue
- Large context amplifies the problem (−46.8% degradation)

**Potential causes:**
- Inefficient runtime decompression path for PTQ1_0
- Poor cache locality when mixing two formats
- Redundant format conversions on each decode cycle
- Memory bandwidth waste from non-contiguous weight access

**Test:** Profile memory access patterns and cache hit rates during decode

**Fix:** Optimize runtime PTQ1_0 decompression or pre-convert at load time

### Hypothesis 3: Launch Path Inconsistency

**Evidence:**
- `STATUS.md` warns about different launch paths producing different numbers
- Python server vs `serve-native` binary can show 37.5% variance
- Thermal drift can cause 32.33–37.31 tok/s spread on identical config

**Test:** Re-measure both models using identical launch path and multiple runs

**Fix:** Use `./splash serve` (Python wrapper) consistently for all measurements

### Hypothesis 4: KV Cache Format Mismatch

**Evidence:**
- Context sensitivity (−46.8% at large context)
- Default int8 KV format may interact poorly with hybrid quantization
- Large prefill time (40.6s for 14.5K tokens) suggests GPU saturation

**Test:** Try different KV formats (`--kv-format bf16`, `--kv-format int4`)

**Fix:** Identify optimal KV format for hybrid model

### Hypothesis 5: Speculation Acceptance Degradation

**Evidence:**
- Hybrid model: 17.5% acceptance rate
- Baseline reported: 18-27% acceptance rate
- Speculation is always-on with 8 verify rows

**Test:** Compare speculation metrics (drafted tokens, accepted tokens, cycles)

**Fix:** Investigate if hybrid quantization affects draft model quality

---

## Recommended Investigation Steps

### Step 1: Verify Configuration (Highest Priority)

```bash
# Check which GGUF the server actually loads
cd /Users/a1111/development/splash_of_mine

# Start hybrid server and check logs
.venv/bin/python -m server.server \
  --model prism-ml/Ternary-Bonsai-2-27B-gguf:hybrid-ptq1pq2 \
  --tokenizer install/models/hybrid-real \
  install/models/hybrid-real --port 8080 2>&1 | tee hybrid_server.log

# Search for GGUF file path in logs
grep -i "gguf\|loading.*model\|weight.*load" hybrid_server.log
```

**Expected:** Should load `Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf`  
**If wrong:** Update `model.json` to reference correct file

### Step 2: Re-Measure Both Models Consistently

```bash
# Use Python measurement script with identical launch path
python3 measure_baseline_simple.py  # Requires manual server start

# Or use automated script (created in investigation)
chmod +x investigate_hybrid_regression.py
python3 investigate_hybrid_regression.py
```

**Key requirements:**
- Use `./splash serve` (Python wrapper) for both models
- Wait 40s warmup time
- Run 3 measurements per configuration
- Calculate mean and standard deviation

### Step 3: Profile Runtime Performance

```bash
# Enable detailed metrics
./splash serve --model <model> --kv-format int8 --ane-telemetry on

# Collect detailed timing from /status endpoint
curl http://127.0.0.1:8080/status | jq .

# Check for anomalies in:
# - decode_tokens_per_second
# - prefill_time_ms
# - memory_bandwidth_used
# - cache_hit_rate
```

### Step 4: Compare GGUF Metadata

```python
# Examine GGUF structure
from install.gguf import Metadata

baseline = Metadata('install/models/.resolved/93ac2e4c.../target/Ternary-Bonsai-2-27B-PQ2_0.gguf', tensors=True)
hybrid = Metadata('install/models/hybrid-real/Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf', tensors=True)

# Count tensors by format
print(f"Baseline PQ2_0: {sum(1 for t in baseline.tensors.values() if t == 142)}")
print(f"Hybrid PTQ1_0: {sum(1 for t in hybrid.tensors.values() if t == 143)}")
print(f"Hybrid PQ2_0: {sum(1 for t in hybrid.tensors.values() if t == 142)}")
```

### Step 5: Test KV Format Variants

```bash
# Test with different KV formats
for kv in int8 int4 bf16; do
  echo "Testing --kv-format $kv"
  ./splash serve --model hybrid-real --kv-format $kv --port 8080 &
  sleep 45
  # Run measurement
  curl -X POST http://127.0.0.1:8080/v1/chat/completions -H "Content-Type: application/json" -d @test_payload.json
  pkill -f "splash serve"
  sleep 5
done
```

---

## Proposed Fixes

### Fix 1: Correct Model Configuration (If Hypothesis 1 Confirmed)

Update `install/models/hybrid-real/model.json` to reference the actual hybrid GGUF:

```json
"target/Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf": {
  "bytes": 6599999999,  // actual size
  "digest": "<actual-sha256>",
  "path": "/Users/a1111/development/splash_of_mine/install/models/hybrid-real/Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf"
}
```

### Fix 2: Pre-Convert PTQ1_0 at Load Time

If runtime conversion is the bottleneck:

```cpp
// In GgufImage.cpp or WeightStore.cpp
// Convert PTQ1_0 → PQ2_0 runtime format at load time
// Store in unified 34-byte format to avoid per-cycle conversion
```

### Fix 3: Optimize Memory Access Pattern

```cpp
// Ensure contiguous weight layout for mixed-format models
// Group PTQ1_0 and PQ2_0 tensors to improve cache locality
// Pre-allocate unified buffer with optimal alignment
```

### Fix 4: Use Proven Launch Path

Always use:
```bash
./splash serve --model prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0
```

Never use:
```bash
build/splash serve-native ...  # Different launch path, incomparable results
```

---

## Conclusion

**Most Likely Root Cause:** Configuration mismatch where `model.json` points to the wrong GGUF file, causing the hybrid model server to load the baseline PQ2_0 model instead of the actual hybrid quantized model.

**Secondary Cause:** If the correct model is loaded, the mixed PTQ1_0/PQ2_0 format may be causing cache inefficiency or redundant runtime conversions, especially visible at large context sizes.

**Next Steps:**
1. Verify which GGUF file is actually loaded by inspecting server logs
2. Correct `model.json` if needed
3. Re-measure both models using identical launch path
4. If regression persists, profile runtime to identify bottleneck
5. Consider abandoning hybrid quantization if no path to parity exists

**Expected Outcome:** If the configuration is corrected and both models use the same launch path, the hybrid model should match baseline performance (both expand to 34 bytes/128 weights at runtime per STATUS.md §3.3).

---

## References

- `STATUS.md` — Authoritative performance data (39.4 tok/s marginal decode, Python server path)
- `HYBRID_BASELINE_DECODE.md` — Original regression report (25.3 tok/s vs 62.9 tok/s baseline)
- `measure_baseline_decode.py` — Test infrastructure with sensor log generator
- `PTQ1_0_IMPLEMENTATION_REPORT.md` — Format specification (superseded, contains errors)
- `docs/archive/PTQ1_0_ANE_BLOCKER.md` — Hybrid GGUF generation details (6.60 GB, 84 PTQ1_0 + 767 PQ2_0)
