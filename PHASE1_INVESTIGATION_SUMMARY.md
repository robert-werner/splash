# Phase 1 Speculation Investigation: Summary

**Investigation Date:** October 19, 2026  
**Branch:** speculation-phase1-impl (planned)  
**Model:** claude-opus-5-5  
**Baseline:** 22% acceptance, 39.4 tok/s decode, ×2.5 speedup  

---

## Key Findings

### 1. Draft Temperature Mechanism Already Exists

**Discovery:** Draft temperature IS already applied in the Metal kernel `draft_select_dflash` (line 1344 of `sampling.metal`):

```metal
float probability = exp((scores[i] - maximum) / params.temperature[batch]);
```

**However:** This temperature comes from the **target model's sampling policy**, not a separate draft-specific parameter.

**Code path:**
```
Target request (temperature=0.0 for greedy)
  ↓
DFlashDraft.cpp: uses target policy
  ↓
DraftSelector.cpp:51: params.temperature[lane] = policies[lane].temperature
  ↓
Metal kernel: applies temperature to codebook distance scores
```

### 2. Root Cause of Suboptimal Acceptance

**Problem:** Draft uses target temperature, which is often 0.0-0.3 for reasoning tasks.

**Impact:** Low temperature → low diversity → predictable but suboptimal draft proposals.

**Hypothesis:** Independent draft temperature of 0.7-0.8 will improve acceptance by 2-4 percentage points.

---

## Implementation Effort Analysis

### Option 1: Compile-Time POC (25 minutes)

**Effort:** 25 minutes total
- 5 min: Edit `runtime/ops/DraftSelector.cpp`, add temperature override constant
- 3 min: Rebuild with `make -j8`
- 2 min: Start server, run 5-prompt benchmark
- Repeat 5x for temperatures: 0.6, 0.7, 0.8, 0.9, 1.0

**Files modified:** 1 file, ~10 lines
**Risk:** None (isolated experiment)
**Deliverable:** Acceptance rate measurements for 5 temperature values

### Option 2: Full Runtime Implementation (1.5 days)

**Effort:** ~8 hours coding + 3 hours testing
- Add `--draft-temperature` and `--draft-top-p` CLI flags
- Thread through: server.py → NativeArguments → DFlashDraft → DraftSelector
- Create separate draft sampling policy
- Test across multiple workloads

**Files modified:** 8 files, ~70 lines
**Risk:** Medium (requires careful threading through call stack)
**Deliverable:** Production-ready `--draft-temperature` flag

---

## Recommendation

### Immediate Action: Execute Compile-Time POC

**Why:**
1. Temperature mechanism already exists (just needs decoupling)
2. 25 minutes provides data to validate hypothesis
3. Informs whether full implementation is worth 1.5 days

**Expected Outcomes:**

| Draft Temp | Expected Acceptance | Decision |
|------------|-------------------|----------|
| 0.6 | 23-24% | Too conservative |
| 0.7 | 24-25% | Likely optimum |
| 0.8 | 24-26% | Likely optimum |
| 0.9 | 23-24% | Too random |
| 1.0 | 22% | Baseline (no improvement) |

### Decision Tree

**If POC shows ≥24% acceptance (2+ point gain):**
- ✅ Proceed to full runtime implementation (1.5 days)
- ✅ Target: 24-28% acceptance, ×2.7 speedup
- ✅ Worth the effort for production deployment

**If POC shows <24% acceptance (<2 point gain):**
- ❌ Skip Phase 1 runtime implementation entirely
- ✅ Proceed directly to Phase 2: Codebook Retraining
- ✅ Target: 32-35% acceptance, ×3.0 speedup (3-4 weeks)

---

## Phase Comparison

| Phase | Acceptance Target | Effort | Gated By | Recommendation |
|-------|------------------|--------|----------|----------------|
| **POC (this investigation)** | **24-26%** | **25 min** | **None** | **✅ Do first** |
| Phase 1 Runtime | 24-28% | 1.5 days | POC success | ⚠️ Conditional |
| Phase 2 Codebooks | 32-35% | 3-4 weeks | — | ✅ Primary path |

---

## Files Created

### 1. PHASE1_IMPLEMENTATION_PLAN.md
Detailed implementation guide covering:
- Code flow analysis
- Full runtime implementation steps (8 files, 70 lines)
- Compile-time POC instructions
- Test protocols
- Validation criteria

### 2. POC_DRAFT_TEMPERATURE.patch
Ready-to-apply patch for compile-time temperature override:
- Modifies `runtime/ops/DraftSelector.cpp`
- Adds `kDraftTemperatureOverride` constant
- Overrides line 51: `params.temperature[lane]`

### 3. test_draft_temperature_poc.py
Benchmark script for POC testing:
- Runs 5 test prompts
- Measures acceptance rate from `/status` endpoint
- Outputs JSON results for each temperature value

---

## Code Trace: Where Temperature Is Used

### Critical Code Locations

1. **Metal Kernel** (`runtime/metal/kernels/decode/sampling.metal:1344`)
   ```metal
   float probability = exp((scores[i] - maximum) / params.temperature[batch]);
   ```
   - Already applies temperature to codebook distance scores
   - Works correctly, just needs different temperature input

2. **Parameter Setup** (`runtime/ops/DraftSelector.cpp:51`)
   ```cpp
   params.temperature[lane] = policies[lane].temperature;
   ```
   - Currently uses target temperature (problem source)
   - POC overrides this with fixed draft temperature
   - Full implementation replaces with draft-specific policy

3. **Policy Source** (`runtime/model/DFlashDraft.cpp:118`)
   ```cpp
   std::span<const ops::SamplingPolicy> policies
   ```
   - Receives target policies from caller
   - No draft-specific policy construction yet
   - Full implementation adds separate draft policy creation

---

## Why Traditional Sampling DOES Apply to DFlash2

**Previous misconception (from PHASE1_SPECULATION_FINDINGS.md):**
> "Traditional sampling parameters (temperature, top-p) don't directly apply to codebook selection"

**Correction:**
Temperature DOES apply and is ALREADY implemented. The Metal kernel uses temperature to convert codebook distance scores into a probability distribution via softmax:

```metal
// Line 1342-1346 in sampling.metal
float maximum = scores[0];
for (uint i = 1; i < Candidates; ++i)
    maximum = max(maximum, scores[i]);
float sum = 0.0f;
for (uint i = 0; i < Candidates; ++i) {
    float probability = exp((scores[i] - maximum) / params.temperature[batch]);
    // ... sampling from this distribution
}
```

This is identical to how temperature works in standard token sampling. The only issue is that it uses the wrong temperature value (target instead of draft-specific).

---

## Exact Steps to Implement POC

### Step 1: Apply Temperature Override (2 minutes)

```bash
cd /Users/a1111/development/splash_of_mine
patch -p1 < POC_DRAFT_TEMPERATURE.patch
```

Or manually edit `runtime/ops/DraftSelector.cpp`:
- Add after line 9: `constexpr float kDraftTemperatureOverride = 0.8f;`
- Replace line 51: `params.temperature[lane] = kDraftTemperatureOverride;`

### Step 2: Rebuild (3 minutes)

```bash
make -j8
```

### Step 3: Test Each Temperature (4 minutes × 5 = 20 minutes)

```bash
# For each temperature: 0.6, 0.7, 0.8, 0.9, 1.0
# 1. Edit kDraftTemperatureOverride in DraftSelector.cpp
# 2. make -j8
# 3. Run benchmark:
./build/splash serve-native ~/models/ternary-bonsai-2-27b-gguf auto auto &
sleep 15
# Run 5 quick generation tests, observe /status acceptance rate
# Kill server
# Record: temperature, acceptance_rate, decode_throughput
```

### Step 4: Compare Results

Create comparison table:
| Temperature | Acceptance | Improvement | Decode tok/s |
|-------------|-----------|-------------|--------------|
| Baseline (target temp) | 22% | — | 39.4 |
| 0.6 | ? | ? | ? |
| 0.7 | ? | ? | ? |
| 0.8 | ? | ? | ? |
| 0.9 | ? | ? | ? |
| 1.0 | ? | ? | ? |

---

## Bottleneck Analysis Validation

The PHASE1_SPECULATION_FINDINGS.md document correctly identified:

✅ **Decode is weight-bandwidth-bound** (7.93 GiB/cycle)  
✅ **Draft proposals are nearly free compute-wise** (cycle time flat across batch sizes)  
✅ **Acceptance rate limited by draft quality** (not width or compute)  

**Additional finding:** Draft quality is currently hampered by using target temperature instead of draft-optimized temperature.

---

## Risk Assessment

### POC Risks: NONE
- Isolated compile-time experiment
- No production impact
- Easily revertible
- 25 minutes of time investment

### Full Implementation Risks: LOW-MEDIUM
- Medium: Regression if wrong temperature defaults chosen
- Low: Performance overhead (temperature already computed)
- Low: ABI compatibility (struct size unchanged)
- Medium: Testing coverage across diverse workloads

**Mitigation:** POC validates hypothesis before committing to full implementation.

---

## Next Steps (In Order)

### Immediate (Next 2 Hours)
1. ✅ **Complete this investigation document** (done)
2. ⏭️ **Review findings with team/lead** (if applicable)
3. ⏭️ **Execute compile-time POC** (25 minutes)
4. ⏭️ **Collect acceptance rate data** for 5 temperatures
5. ⏭️ **Analyze results** and make Phase 1 vs Phase 2 decision

### If POC Shows ≥2 Point Gain (Day 2-3)
1. Implement full runtime `--draft-temperature` flag (1.5 days)
2. Test across diverse workloads
3. Document optimal temperature range (likely 0.7-0.8)
4. Deploy with default `--draft-temperature=0.8`

### If POC Shows <2 Point Gain (Week 2+)
1. Skip Phase 1 entirely
2. Begin Phase 2: Codebook Retraining planning
3. Set up data collection pipeline (10K-20K inference samples)
4. Retrain predecessor/successor codebooks on target model's distribution
5. Target: 32-35% acceptance in 3-4 weeks

---

## Conclusion

**Phase 1 speculation tuning IS feasible** and requires minimal implementation:

✅ **Temperature mechanism already exists** in the codebase  
✅ **Only needs decoupling** from target temperature  
✅ **25-minute POC** validates hypothesis before 1.5-day commitment  
✅ **Expected improvement:** 2-4 percentage points (22% → 24-26%)  
✅ **Clear decision criteria** for proceeding to full implementation  

**Corrected Understanding:**
- DFlash2 codebook selection DOES use temperature (not "fixed decoding")
- Temperature applies to codebook distance scores via softmax
- Current problem: using target temperature instead of draft-optimized temperature

**Recommended Action:**
Execute compile-time POC immediately (25 minutes) to gather data for Phase 1 vs Phase 2 decision.

---

**Files Delivered:**
1. `PHASE1_IMPLEMENTATION_PLAN.md` — Detailed implementation guide
2. `POC_DRAFT_TEMPERATURE.patch` — Ready-to-apply temperature override patch
3. `test_draft_temperature_poc.py` — Benchmark script for POC testing
4. `PHASE1_INVESTIGATION_SUMMARY.md` — This executive summary
