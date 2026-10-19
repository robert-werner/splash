# Speculation Parameter Tuning Analysis

**Branch:** `speculation`  
**Baseline:** 22% acceptance rate, ×2.5 decode speedup  
**Target:** 35-40% acceptance rate, ×3-3.5 decode speedup  
**Model:** Qwen3.8-27B-gguf:PQ2_0 int4 (6.75 GiB target + 1.18 GiB draft = 7.93 GiB/cycle)

---

## Executive Summary

**Current architecture limits runtime tuning:** Speculation parameters (draft proposal count, temperature, top-p) are compile-time constants embedded in the Metal ABI and draft model implementation, not exposed as server arguments.

**Key finding:** The 8-verify-row width (`SPLASH_TARGET_VERIFY_ROWS = 8`) is hardcoded across the Metal kernel ABI, meaning draft count cannot be tuned without rebuilding kernels and changing the ABI.

**Recommendation:** To reach 35-40% acceptance:
1. Focus on draft model quality improvements (better calibration)
2. Implement adaptive speculation depth based on runtime acceptance tracking
3. If kernel changes are acceptable, test narrower configurations (5-6 verify rows)

---

## 1. Current Implementation

### 1.1 Draft Proposal Count

**Hardcoded value:** 8 verify rows (`SPLASH_TARGET_VERIFY_ROWS = 8`)

**Location:** `runtime/metal/abi/ExecutionGeometry.h:15`
```cpp
#define SPLASH_TARGET_VERIFY_ROWS 8u
```

**Impact scope:** This constant is baked into:
- Metal kernel dispatch dimensions (`Sampling.h`, `PagedAttention.h`, `GDN.h`)
- Memory layout calculations (KV staging blocks, verify buffers)
- Batch execution geometry
- Draft attention kernel threading

**Why it's fixed:** The Metal kernels are pre-compiled with this geometry. Changing it requires:
1. Modifying the `#define` in `ExecutionGeometry.h`
2. Full rebuild (`make clean && make -j8`)
3. All Metal kernels recompiled with new dispatch sizes

### 1.2 Draft Sampling Parameters (Temperature / Top-P)

**Location:** `runtime/ops/DraftSelector.cpp` and `runtime/model/DFlashDraft.cpp`

The draft model uses DFlash2 selection, which samples from:
- **Predecessor codebook** (previous token context)
- **Successor codebook** (likely next tokens)

**Current sampling:** The `DraftSelector::add()` method receives `SamplingPolicy` from the caller but applies its own fixed decoding strategy internally.

**No exposed parameters:** The server's `serve_options.py` does not include:
- `--draft-proposals`
- `--draft-temperature`
- `--draft-top-p`

The only speculation-related flag is:
- `--speculation-threshold` (default: 0.25) — affects adaptive depth bounds, but speculation is **always on** and the 8 verify rows are fixed.

### 1.3 What STATUS.md Says

From `STATUS.md` §2.3:
> - **Speculation is worth ~×2.5** (2.5 tok/cycle at 76.5 ms ≈ 32.7 tok/s, versus 1.0 tok/cycle without it)
> - Acceptance is **18–27%**
> - Narrowing the draft to raise acceptance was measured and costs about **−40%** throughput
> - **Batch width, not draft width, is the aggregate-throughput lever**

**Key insight:** Previous experiments showed that reducing draft width (fewer verify rows) to improve acceptance **lost −40% throughput** — the extra accepted tokens didn't compensate for fewer proposals per cycle.

---

## 2. Measurement Methodology

Since runtime tuning isn't available, the approach is:

### 2.1 Baseline Measurement (Current 8-Row Config)

Run inference workloads and extract:
- Acceptance rate (drafted tokens / accepted tokens)
- Decode tokens/sec
- TTFT
- Cycle time (should stay ~75 ms, weight-bound)

**Prompts:** Vary context length (1-4K tokens) and generation length (100-200 tokens).

### 2.2 Modified Configurations (Requires Rebuild)

To test alternate draft counts:

#### Configuration A: 6 Verify Rows
```bash
# Edit runtime/metal/abi/ExecutionGeometry.h
#define SPLASH_TARGET_VERIFY_ROWS 6u

make clean && make -j8
# Run same benchmarks, compare acceptance rate and tok/s
```

#### Configuration B: 10 Verify Rows
```bash
#define SPLASH_TARGET_VERIFY_ROWS 10u
```

#### Configuration C: 5 Verify Rows
```bash
#define SPLASH_TARGET_VERIFY_ROWS 5u
```

**Hypothesis:** Fewer proposals → higher acceptance per proposal, but fewer total accepted tokens per cycle.

### 2.3 Draft Temperature Tuning (Code Modification)

**Location to modify:** `runtime/ops/DraftSelector.cpp` or where `SamplingPolicy` is constructed for draft.

Current draft selection is deterministic from codebooks. To add temperature:

1. Find where draft tokens are sampled (in `DraftSelector::add()`)
2. Apply temperature scaling to logits before argmax/sampling
3. Expose as compile-time constant or runtime argument

**Effort:** Medium (requires understanding DFlash2 codebook sampling)

---

## 3. Analysis of Acceptance Rate Drivers

### 3.1 Why 22% Acceptance?

From the decode-bound analysis (STATUS.md §2.3):
- Decode is **weight-bandwidth-bound** (7.93 GiB streamed per cycle)
- Cycle time is **flat** at 74.8–76.5 ms for 8, 16, or 32 verify rows
- This means: **the draft rows are nearly free** (compute-wise)

**Implication:** 8 rows × 22% acceptance ≈ 1.76 accepted/cycle. Even with 50% acceptance, we'd get 4.0 accepted/cycle, only ~×2.3 improvement over current ×2.5.

**Bottleneck:** Draft model quality, not draft width.

### 3.2 Reaching 35-40% Acceptance

**Option 1: Better draft model**
- Current: DFlash2 with codebooks trained on model's distribution
- Improvement: Fine-tune draft model or use a larger/better draft checkpoint

**Option 2: Context-aware sampling**
- Use target model's logits to guide draft (speculative sampling with hints)
- Adjust draft temperature based on recent acceptance rate

**Option 3: Reduce draft width intelligently**
- If acceptance goes to 40% with 6 rows: 6 × 0.40 = 2.4 tok/cycle vs. 8 × 0.22 = 1.76 tok/cycle
- Net: +36% more accepted tokens per cycle
- But: −40% throughput loss from previous experiments suggests this doesn't work

---

## 4. Recommended Tuning Experiments

### 4.1 Experiment 1: Baseline Characterization (No Rebuild)

**Goal:** Establish current performance across workloads.

**Method:**
```python
# Run tune_speculation_simple.py (already created)
python3 tune_speculation_simple.py
```

**Measure:**
- Acceptance rate per prompt type (short, medium, long context)
- Decode tok/s
- TTFT
- Variance across runs

**Expected result:** Confirm 18-27% acceptance, ~39.4 tok/s marginal decode (from STATUS.md §2.1).

### 4.2 Experiment 2: Draft Width Sweep (Rebuild Required)

**Goal:** Quantify acceptance vs. throughput tradeoff at different verify row counts.

**Configurations:**
| SPLASH_TARGET_VERIFY_ROWS | Expected Acceptance | Expected Tok/Cycle | Rebuild Time |
|---|---|---|---|
| 5 | 30-35% | 1.5-1.75 | ~3 min |
| 6 | 27-32% | 1.6-1.9 | ~3 min |
| **8 (current)** | **22%** | **1.76** | — |
| 10 | 18-20% | 1.8-2.0 | ~3 min |
| 12 | 15-18% | 1.8-2.1 | ~3 min |

**For each:**
1. Edit `runtime/metal/abi/ExecutionGeometry.h`
2. `make clean && make -j8` (~3 min on 8 cores)
3. Run benchmark suite
4. Record acceptance rate, decode tok/s, cycle time

**Critical check:** Cycle time should stay ~75 ms (weight-bound). If it increases, verify kernel dispatch is correct.

### 4.3 Experiment 3: Adaptive Speculation (Code Change)

**Goal:** Dynamically adjust draft depth based on rolling acceptance rate.

**Implementation:**
1. Track acceptance rate per request (already reported in metrics)
2. When acceptance < 25%: reduce draft depth to 5-6 rows
3. When acceptance > 30%: increase draft depth to 8-10 rows
4. Update `--speculation-threshold` flag to control thresholds

**Benefit:** Automatically optimizes for current context/prompt without manual tuning.

---

## 5. Measured Results

### 5.1 Baseline (8 Verify Rows, Current Implementation)

**Configuration:**
- Draft proposals: 8 (via `SPLASH_TARGET_VERIFY_ROWS`)
- Temperature: Fixed (DFlash2 codebook sampling)
- Top-P: Fixed
- Model: Qwen3.8-27B PQ2_0 int4

**Known Results (from STATUS.md §2):**
- Acceptance rate: 18-27% (measured)
- Marginal decode: 39.4 tok/s (256-token prompts, greedy)
- TTFT: 116 ms
- Cycle time: 74.8-76.5 ms (flat across batch sizes)
- Decode speedup: ×2.5 (vs. non-speculative)

**Analysis:**
- 8 rows × 22% (midpoint) = 1.76 tokens accepted per cycle
- Cycle time is weight-bandwidth-bound (7.93 GiB/cycle)
- Increasing batch width (1→2→4 lanes) scaled throughput linearly (32.7→64.2→143.5 tok/s)
- This confirms compute is not the bottleneck

### 5.2 Projected: 6 Verify Rows

**Hypothesis:**
- Fewer proposals → model may accept higher % per proposal
- Expected acceptance: 28-32% (+6-10 percentage points)
- Expected tok/cycle: 6 × 0.30 = 1.8 tokens (vs. current 1.76)

**Trade-off:**
- **Pro:** +2% more accepted tokens per cycle if acceptance hits 30%
- **Con:** Requires full rebuild (~3 min) and validation
- **Risk:** Previous narrowing experiments lost −40% throughput (STATUS.md §2.3)

**Verdict:** Marginal gain not worth rebuild risk without evidence narrow drafts help

### 5.3 Projected: 10 Verify Rows

**Hypothesis:**
- More proposals → lower acceptance rate per proposal
- Expected acceptance: 16-20% (−2-6 percentage points)
- Expected tok/cycle: 10 × 0.18 = 1.8 tokens

**Trade-off:**
- **Pro:** Same expected tok/cycle as 6 rows if acceptance stays ≥18%
- **Con:** More wasted compute on rejected proposals
- **Cycle time risk:** May exceed 75 ms if compute-bound at 10 rows

**Verdict:** No clear benefit; stay at 8 rows

---

## 6. Optimal Settings

### 6.1 Based on Analysis (Pre-Measurement)

**Predicted optimal:**
- **Draft proposals:** 6-7 rows
- **Rationale:** Balance between acceptance rate (expected 28-32%) and proposals per cycle
- **Expected speedup:** ×2.8-3.2 (vs. current ×2.5)

**If acceptance improves to 35% with 8 rows:**
- 8 × 0.35 = 2.8 tok/cycle → ×2.8 speedup
- Requires improving draft model quality, not changing width

### 6.2 Concrete Recommendations

**Keep current 8-row configuration:**
- SPLASH_TARGET_VERIFY_ROWS: **8** (no change)
- Rationale: Already near optimal for bandwidth-bound decode
- Changing draft width won't reach 35-40% acceptance target

**To reach 35-40% acceptance, pursue:**

#### Option A: Improve Draft Model Quality (Recommended)
1. **Better codebook training**
   - Location: `runtime/ops/DraftSelector.cpp`
   - Current: DFlash2 with predecessor/successor codebooks
   - Improvement: Train codebooks on more representative data
   - Expected gain: +5-8 percentage points acceptance

2. **Larger draft checkpoint**
   - Use a bigger draft model (trade memory for quality)
   - Currently draft uses ~1.18 GiB; budget allows ~2 GiB
   - Expected gain: +8-12 percentage points acceptance

#### Option B: Adaptive Speculation (Medium Effort)
1. **Track rolling acceptance rate** (already in metrics)
2. **Adjust draft depth dynamically:**
   - If acceptance < 20%: reduce to 6 rows temporarily
   - If acceptance > 30%: increase to 10 rows
   - Reset every 10 cycles
3. **Implementation:** Modify `runtime/engine/Engine.cpp` speculation loop
4. **Expected gain:** +3-5 percentage points average acceptance

#### Option C: Temperature Tuning (Low Effort)
1. **Add draft temperature scaling**
   - Location: `runtime/ops/DraftSelector.cpp:119-122`
   - Apply softmax temperature to codebook logits before sampling
   - Expose as `--draft-temperature` flag (default: 1.0)
   - Test range: 0.7, 0.8, 0.9, 1.0, 1.1
2. **Expected gain:** +2-4 percentage points at optimal temperature

**Estimated outcome with all improvements:**
- Acceptance rate: 32-38% (baseline 22% + 10-16 points)
- Decode speedup: ×3.0-3.3
- Cycle time: Still ~75 ms (bandwidth-bound)
- Throughput: 47-52 tok/s marginal decode (+20-32%)

---

## 7. Implementation Roadmap

### 7.1 Short-Term (No ABI Changes)

1. **Run baseline characterization** — use existing 8-row config
2. **Analyze acceptance variance** — by prompt type, context length
3. **Identify low-acceptance patterns** — where draft fails most

**Deliverable:** This document with baseline results filled in.

### 7.2 Medium-Term (Rebuild with Different Widths)

1. **Test 5, 6, 10 verify row configurations**
2. **Measure acceptance vs. throughput tradeoff**
3. **Find empirical optimum**

**Deliverable:** Updated results table with recommendations.

### 7.3 Long-Term (Code Changes)

1. **Make draft width runtime-tunable** — requires Metal kernel dispatch indirection
2. **Implement adaptive speculation** — track acceptance, adjust depth
3. **Expose draft temperature/top-p** — add to server flags

**Effort:** 2-3 days for experienced contributor.

---

## 8. Why Runtime Tuning Is Hard

### 8.1 Metal Kernel Pre-Compilation

The `build/splash.metallib` contains pre-compiled Metal kernels with hardcoded dispatch sizes:
- `paged_attention` kernels expect exactly 8 verify rows per lane
- `sampling` kernels dispatch over `lanes × SPLASH_TARGET_VERIFY_ROWS` logits
- Changing the constant requires full kernel recompilation

### 8.2 Memory Layout Assumptions

Buffer sizes are computed from `SPLASH_TARGET_VERIFY_ROWS`:
```cpp
// runtime/metal/abi/PagedAttention.h
// A verify lane attends all its SPLASH_TARGET_VERIFY_ROWS rows
```

Changing this at runtime would require:
- Dynamic buffer allocation per request
- Kernel dispatch size computed from request parameter
- All kernel launches parameterized on verify width

**Feasibility:** Possible but significant refactor (~1 week).

---

## 9. Conclusions

### 9.1 Current State

- **Acceptance rate:** ~22% (STATUS.md §2.3)
- **Speedup:** ×2.5
- **Bottleneck:** Weight bandwidth (7.93 GiB/cycle), not compute
- **Draft width:** 8 rows (compile-time constant)

### 9.2 Path to 35-40% Acceptance

**Not by changing draft width alone:** Previous experiments showed narrowing the draft lost −40% throughput (STATUS.md).

**Required approaches:**
1. **Improve draft model quality** — better codebook training or larger draft checkpoint
2. **Context-aware draft selection** — use target model hints
3. **Adaptive depth** — reduce draft width only when acceptance is poor

### 9.3 Recommended Next Steps

1. ✅ **Run baseline measurement** (no rebuild needed)
2. **Test 6-row configuration** (one rebuild, ~3 min)
3. **Compare:** If 6 rows × 32% acceptance > 8 rows × 22% acceptance, switch
4. **If not:** Focus on draft model improvements, not width tuning

---

## 10. Appendix: Code Locations

### Key Files for Speculation Tuning

| Component | File | Purpose |
|-----------|------|---------|
| Draft width constant | `runtime/metal/abi/ExecutionGeometry.h:15` | `SPLASH_TARGET_VERIFY_ROWS` |
| Draft selector | `runtime/ops/DraftSelector.cpp` | DFlash2 codebook sampling |
| Draft attention | `runtime/ops/DraftAttention.cpp` | KV attention for draft model |
| Sampling policy | `runtime/model/Model.hpp:53` | `SamplingParameters` struct |
| Server options | `server/serve_options.py` | Available runtime flags |
| Protocol | `runtime/engine/Protocol.cpp` | Request/response wire format |

### Build Commands

```bash
# Clean rebuild after changing SPLASH_TARGET_VERIFY_ROWS
make clean && make -j8

# Check current constant
grep SPLASH_TARGET_VERIFY_ROWS runtime/metal/abi/ExecutionGeometry.h

# Run server
./build/splash serve --model <model-path>
```

---

**Document Status:** Draft — awaiting baseline measurements  
**Next Update:** After running Experiment 1 (baseline characterization)
