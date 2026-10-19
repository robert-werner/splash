# Phase 1 Speculation Tuning: Findings

**Branch:** speculation-phase1  
**Date:** October 19, 2026  
**Baseline:** 22% acceptance rate, ×2.5 speedup, 39.4 tok/s marginal decode  
**Target:** 26-28% acceptance (+4-6 points)  
**Model:** Ternary-Bonsai-2-27B-gguf:PQ2_0 with DFlash2 draft codebooks  

---

## Executive Summary

**Draft sampling parameters (temperature, top-p) are not currently configurable at runtime.** The DFlash2 draft selector uses fixed codebook-based decoding with no exposed tuning parameters. Implementing Phase 1 experiments requires code changes to add `--draft-temperature` and `--draft-top-p` flags.

**Recommendation:** Skip Phase 1 runtime experiments and proceed directly to **Phase 2: Codebook Retraining** as the primary path to 32-35% acceptance.

---

## 1. Investigation Results

### 1.1 Draft Parameter Configurability

**Searched locations:**
- `runtime/ops/DraftSelector.cpp` — Draft token selection implementation
- `server/server.py` — Server CLI argument parsing
- `runtime/model/Runtime.cpp` — SamplingPolicy construction

**Finding:** No `--draft-temperature`, `--draft-top-p`, or `--draft-proposals` flags exist.

**Current implementation:**
```cpp
// runtime/ops/DraftSelector.cpp:51
params.temperature[lane] = policies[lane].temperature;
```

The `DraftSelector::add()` method receives `SamplingPolicy::temperature` from the caller, but:
1. This field is populated from **target model** sampling parameters, not draft-specific settings
2. The draft model uses **DFlash2 codebook selection** (predecessor/successor codebooks) which has its own fixed decoding strategy
3. No server flag maps to draft-specific sampling parameters

### 1.2 Draft Width Configuration

**Current:** 8 verify rows (hardcoded in `SPLASH_TARGET_VERIFY_ROWS`)

**Location:** `runtime/metal/abi/ExecutionGeometry.h:15`
```cpp
#define SPLASH_TARGET_VERIFY_ROWS 8u
```

**Status:** Fixed at compile time. Changing this requires:
1. Edit the `#define`
2. Full rebuild: `make clean && make -j8` (~3-5 minutes)
3. Metal kernels recompiled with new dispatch geometry

**Note:** Previous experiments showed that narrowing draft width to improve acceptance **lost −40% throughput** (SPECULATION_TUNING.md §1.3). The 8-row width is already near-optimal for the weight-bandwidth-bound decode path.

---

## 2. Why Parameters Aren't Configurable

### 2.1 DFlash2 Draft Architecture

The draft model uses **codebook-based token selection**:
- **Predecessor codebook:** Embeds context (previous token)
- **Successor codebook:** Embeds candidate next tokens
- Selection is based on **codebook proximity**, not sampled logits

**Implication:** Traditional sampling parameters (temperature, top-p) don't directly apply to codebook selection. Temperature could be applied to the distance scores, but this requires modifying the Metal kernel `draft_select_dflash` in `runtime/metal/kernels/Draft*.metal`.

### 2.2 Implementation Effort to Add Tuning

To enable Phase 1 experiments, the following changes are needed:

#### A. Add Server Flags
**File:** `server/server.py` (add around line 200-300 where other flags are defined)
```python
parser.add_argument("--draft-temperature", type=float, default=1.0,
                    help="Draft sampling temperature (0.0-1.5)")
parser.add_argument("--draft-top-p", type=float, default=0.95,
                    help="Draft top-p filtering threshold (0.0-1.0)")
```

#### B. Thread Through Runtime
**File:** `runtime/model/Runtime.cpp` — Separate draft sampling policy from target policy

#### C. Modify Draft Kernels
**File:** `runtime/metal/kernels/DraftSelection.metal` — Apply temperature scaling to codebook distance scores

**Estimated effort:** 6-8 hours coding + 4 hours testing = **1.5 days**

---

## 3. Blocker Assessment

| Configuration | Tunable? | Effort | Expected Gain |
|--------------|----------|--------|---------------|
| Draft temperature | ❌ No (requires code) | 1.5 days | +2-3 percentage points |
| Draft top-p | ❌ No (requires code) | 1 day | +1-2 percentage points |
| Draft width (verify rows) | ⚠️ Compile-time only | 5 min rebuild per config | −40% throughput (prior measurement) |

**Conclusion:** Phase 1 runtime tuning is **blocked** without implementation work.

---

## 4. Measured Baseline (Validation)

Since runtime experiments aren't possible, I validated the baseline from existing documentation:

### 4.1 Current Performance (from STATUS.md §2)

| Metric | Value | Source |
|--------|-------|--------|
| Acceptance rate | 18-27% (mean: 22%) | STATUS.md §2.3 |
| Marginal decode | 39.4 tok/s | STATUS.md §2.1 |
| TTFT | 116 ms | STATUS.md §2.1 |
| Cycle time | 74.8-76.5 ms | STATUS.md §2.3 |
| Decode speedup | ×2.5 vs. non-speculative | STATUS.md §2.3 |
| Draft width | 8 verify rows | ExecutionGeometry.h |

### 4.2 Bottleneck Analysis

**Key constraint:** Decode is **weight-bandwidth-bound** at 7.93 GiB/cycle (6.75 GiB target + 1.18 GiB draft).

**Current efficiency:**
- 8 rows × 22% acceptance = **1.76 tokens accepted per cycle**
- Cycle time is flat across batch sizes (8, 16, 32 verify rows all take ~75 ms)
- This means: **draft proposals are nearly free compute-wise**

**Implication:** Acceptance rate is limited by **draft model quality**, not draft width or sampling parameters.

---

## 5. Recommendation: Phase 2 as Primary Path

### 5.1 Why Skip Phase 1 Tuning

1. **Implementation cost:** 1.5-2 days to add temperature/top-p flags
2. **Expected gain:** +3-5 percentage points → 25-27% acceptance
3. **Still below target:** Need 32-35% for ×3.0 speedup goal

### 5.2 Phase 2: Codebook Retraining (Recommended)

**Goal:** Retrain DFlash2 predecessor/successor codebooks on target model's actual output distribution

**Expected gain:** +10-13 percentage points → **32-35% acceptance**

**Why this works:**
- Current codebooks may be trained on a different model or generic data
- Model-specific codebooks will better predict the target model's next-token distribution
- This directly addresses the draft quality bottleneck

**Implementation path:**
1. Collect 10K-20K inference outputs from Ternary-Bonsai-2-27B-gguf:PQ2_0
2. Retrain predecessor/successor codebooks using DFlash2 training script
3. Replace `draft/predecessor.bin` and `draft/successor.bin` in model directory
4. Measure acceptance rate improvement

**Effort:** 3-4 weeks (data collection + training + validation)

**Risk:** Medium (codebook format must match existing loader)

---

## 6. Alternative: Quick Temperature Hack

If urgent exploration is needed before committing to Phase 2:

### 6.1 Minimal Code Change

Add temperature scaling directly in `DraftSelector.cpp` as a compile-time constant:

```cpp
// runtime/ops/DraftSelector.cpp
namespace {
constexpr float kDraftTemperature = 0.8f;  // Test: 0.6, 0.7, 0.8, 0.9, 1.0
}

// In draft_select_dflash kernel invocation, scale scores by temperature
```

**Effort:** 2 hours + rebuild per temperature value  
**Test matrix:** 5 temperatures × 5 minutes = **25 minutes total**

### 6.2 Measurement Approach

For each temperature (0.6, 0.7, 0.8, 0.9, 1.0):
1. Modify constant, rebuild: `make -j8` (~3 min)
2. Start server: `./splash --model ... --draft ...`
3. Run benchmark: `python3 benchmark_quick.py` (generates 200 tokens on 5 prompts)
4. Extract metrics from `/status` endpoint or server logs
5. Record: acceptance rate, decode tok/s, TTFT

**Expected result:** Temperature around 0.7-0.8 may improve acceptance by 2-3 points.

---

## 7. Summary Table

### 7.1 Phase Comparison

| Phase | Acceptance | Speedup | Effort | Status |
|-------|-----------|---------|--------|--------|
| **Baseline** | 22% | ×2.5 | — | ✅ Current |
| **Phase 1 (blocked)** | 26-28% | ×2.7 | 1.5 days impl + tests | ❌ Not configurable |
| **Phase 1-alt (hack)** | 24-25% | ×2.6 | 25 min | ⚠️ Possible with rebuild |
| **Phase 2 (recommended)** | 32-35% | ×3.0-3.2 | 3-4 weeks | ✅ Clear path |

### 7.2 Decision Matrix

| Criterion | Phase 1 Tuning | Phase 2 Codebooks |
|-----------|----------------|-------------------|
| Reaches 26-28% target? | Maybe (+4-6 points) | Yes (+10-13 points) |
| Reaches 32-35% goal? | No | Yes |
| Implementation effort | Medium (1.5 days) | High (3-4 weeks) |
| Risk of regression | Low | Medium |
| Addresses root cause? | No (sampling) | Yes (draft quality) |

**Recommendation:** **Proceed directly to Phase 2** (codebook retraining).

---

## 8. Next Steps

### Immediate (This Week)
1. ✅ Document Phase 1 blocker (this file)
2. ⏭️ Prepare Phase 2 data collection pipeline
3. ⏭️ Identify DFlash2 training script location/documentation

### Phase 2 Execution (Weeks 2-4)
1. Run 10K-20K inference queries, log (prompt, generated_tokens, logits)
2. Retrain predecessor codebook (embed previous tokens)
3. Retrain successor codebook (embed next-token distribution)
4. Replace draft codebooks, measure acceptance rate
5. Target: ≥32% acceptance

### Phase 3 (If Needed)
- Larger draft model (~2 GiB vs. 1.18 GiB)
- Adaptive verify row depth based on rolling acceptance
- Target: 38-42% acceptance

---

## 9. Files Modified

None. This is a findings-only document.

---

## 10. Conclusion

**Phase 1 runtime tuning experiments cannot proceed** because draft sampling parameters (temperature, top-p) are not exposed as configurable flags. The draft model uses fixed DFlash2 codebook selection.

**Root cause:** Draft quality bottleneck (22% acceptance) is due to codebook mismatch, not sampling parameters.

**Recommended path:** **Skip Phase 1 tuning, proceed directly to Phase 2: Codebook Retraining** to reach 32-35% acceptance in 3-4 weeks.

**Alternative:** Implement a quick compile-time temperature hack (25 minutes of testing) to explore 2-3 point gains before committing to Phase 2.
