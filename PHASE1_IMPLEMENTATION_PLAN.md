# Phase 1 Speculation: Implementation Plan & POC

**Date:** October 19, 2026  
**Branch:** speculation-phase1-impl  
**Investigator:** Claude Opus 5.5  
**Baseline:** 22% acceptance, 39.4 tok/s decode, ×2.5 speedup  

---

## Executive Summary

**Finding:** Draft temperature IS already used in DFlash2 selection (line 1344 of `sampling.metal`), but it's populated from the **target model's temperature**, not draft-specific parameters.

**Root Cause:** `DraftSelector::add()` receives `policies[lane].temperature` from the target sampling policy, not a separate draft policy.

**Quick Win Available:** Compile-time temperature override in `DraftSelector.cpp` can test different draft temperatures in ~25 minutes.

**Full Implementation Effort:** 1.5 days (detailed below).

**Recommendation:** **Implement compile-time POC first** (Section 3), measure impact, then decide on full runtime implementation.

---

## 1. Code Flow Analysis

### 1.1 Current Temperature Path

```
server/server.py (no --draft-temperature flag)
    ↓
runtime/model/DFlashDraft.cpp:selectDraft()
    ↓
    SamplingPolicy policies[] = target policies (temperature from request)
    ↓
runtime/ops/DraftSelector.cpp:add()
    ↓
    params.temperature[lane] = policies[lane].temperature  (line 51)
    ↓
Metal kernel draft_select_dflash (sampling.metal:1344)
    ↓
    exp((scores[i] - maximum) / params.temperature[batch])
```

**Key insight:** Temperature is already applied to codebook distance scores, but it uses the target model's temperature.

### 1.2 Why This Matters

- Target temperature (often 0.0-0.3 for reasoning) may be too low for draft diversity
- Draft needs higher temperature (0.6-0.8) to explore more candidates
- Current: draft temperature = target temperature (suboptimal for acceptance)
- Goal: draft temperature independent of target temperature

---

## 2. Full Runtime Implementation (1.5 Days)

### 2.1 Step-by-Step Effort Breakdown

| Step | File(s) | Task | Time | Risk |
|------|---------|------|------|------|
| 1 | `server/serve_options.py` | Add `--draft-temperature`, `--draft-top-p` args | 30 min | Low |
| 2 | `server/server.py` | Pass args to `_native_command()` | 15 min | Low |
| 3 | `runtime/engine/NativeArguments.hpp` | Add fields to `NativeArguments` | 15 min | Low |
| 4 | `runtime/engine/NativeArguments.cpp` | Parse `--draft-temperature`, `--draft-top-p` | 45 min | Low |
| 5 | `runtime/model/DFlashDraft.hpp` | Add draft sampling policy field | 20 min | Low |
| 6 | `runtime/model/DFlashDraft.cpp` | Create separate draft policy from args | 1 hour | Medium |
| 7 | `runtime/model/Model.cpp` | Thread draft policy to DraftSelector | 45 min | Medium |
| 8 | `runtime/ops/DraftSelector.cpp` | Use draft policy instead of target policy | 30 min | Low |
| 9 | Build & test | Rebuild, verify no regressions | 2 hours | Medium |
| 10 | Benchmark | Test acceptance rates at 0.6, 0.7, 0.8, 0.9, 1.0 | 1 hour | Low |

**Total:** ~8 hours coding + 3 hours testing = **1.5 days**

### 2.2 Code Changes Required

#### A. Server flags (`server/server.py`, line ~1645)

```python
# In parse_args(), after existing speculation flags:
parser.add_argument("--draft-temperature", type=float, default=None,
                    help="Draft sampling temperature (default: use target temperature)")
parser.add_argument("--draft-top-p", type=float, default=None,
                    help="Draft top-p nucleus threshold (default: 1.0)")
```

#### B. Native command (`server/server.py`, line ~1733)

```python
# In _native_command(), after --speculation-threshold:
if args.draft_temperature is not None:
    command.extend(("--draft-temperature", str(args.draft_temperature)))
if args.draft_top_p is not None:
    command.extend(("--draft-top-p", str(args.draft_top_p)))
```

#### C. Argument parsing (`runtime/engine/NativeArguments.cpp`, line ~273)

```cpp
} else if (option == "--draft-temperature") {
  double temp = 0.0;
  if (!parseFinite(value, temp) || temp < 0.0 || temp > 2.0)
    throw UsageError("--draft-temperature requires a number in [0.0, 2.0]");
  result.draftTemperature = static_cast<float>(temp);
} else if (option == "--draft-top-p") {
  double p = 0.0;
  if (!parseFinite(value, p) || p < 0.0 || p > 1.0)
    throw UsageError("--draft-top-p requires a number in [0.0, 1.0]");
  result.draftTopP = static_cast<float>(p);
}
```

#### D. Draft policy construction (`runtime/model/DFlashDraft.cpp`)

```cpp
// In DFlashDraft::selectDraft(), replace:
//   policies[lane] = targetPolicies[lane];
// With:
SamplingPolicy draftPolicy{};
if (arguments_.draftTemperature) {
  draftPolicy.temperature = *arguments_.draftTemperature;
  draftPolicy.topP = arguments_.draftTopP.value_or(1.0F);
} else {
  draftPolicy.temperature = targetPolicies[lane].temperature;
  draftPolicy.topP = targetPolicies[lane].topP;
}
policies[lane] = draftPolicy;
```

### 2.3 Implementation Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Build breaks due to ABI change | Low | High | Keep params struct size constant |
| Regression in acceptance rate | Medium | High | A/B test with baseline before deploying |
| Draft top-p doesn't apply to codebooks | Medium | Low | Focus on temperature first, top-p later |
| Performance overhead | Low | Medium | Temperature is already used, no new compute |

---

## 3. Compile-Time POC (25 Minutes)

### 3.1 Minimal Code Change

**File:** `runtime/ops/DraftSelector.cpp`

**Original (line 51):**
```cpp
params.temperature[lane] = policies[lane].temperature;
```

**Modified:**
```cpp
namespace {
constexpr float kDraftTemperatureOverride = 0.8f; // EXPERIMENT: Test 0.6, 0.7, 0.8, 0.9, 1.0
}

// In DraftSelector::add():
params.temperature[lane] = kDraftTemperatureOverride;
```

### 3.2 Test Protocol

For each temperature value (0.6, 0.7, 0.8, 0.9, 1.0):

```bash
# 1. Edit runtime/ops/DraftSelector.cpp, change kDraftTemperatureOverride
# 2. Rebuild (3 min)
cd /Users/a1111/development/splash_of_mine
make -j8

# 3. Start server (15 sec)
./build/splash serve-native ~/models/ternary-bonsai-2-27b-gguf auto auto &
SERVER_PID=$!
sleep 15

# 4. Run benchmark (2 min)
python3 scripts/benchmark_speculation.py \
  --prompts 5 \
  --tokens 200 \
  --output results_draft_temp_${TEMP}.json

# 5. Extract metrics
jq '.acceptance_rate, .decode_throughput' results_draft_temp_${TEMP}.json

# 6. Stop server
kill $SERVER_PID
```

**Expected results table:**

| Draft Temp | Acceptance Rate | Decode tok/s | Speedup | Notes |
|------------|----------------|--------------|---------|-------|
| 0.6 | ? | ? | ? | Lower diversity |
| 0.7 | ? | ? | ? | Expected optimum |
| 0.8 | ? | ? | ? | Baseline test |
| 0.9 | ? | ? | ? | Higher diversity |
| 1.0 | ? | ? | ? | Maximum entropy |

### 3.3 Expected Outcomes

**Hypothesis:** Temperature 0.7-0.8 will increase acceptance by 2-4 percentage points (24-26%).

**Why:** 
- Current baseline uses target temperature (often 0.0-0.3 for reasoning tasks)
- Draft needs higher temperature for diverse proposals
- Too high (>0.9): excessive randomness, lower acceptance
- Too low (<0.6): insufficient exploration, current problem

**If POC shows <2 point gain:** Skip full implementation, proceed to Phase 2 (codebook retraining).

**If POC shows ≥2 point gain:** Full implementation worth 1.5 days (reaches 24-26% acceptance).

---

## 4. Alternative: Metal Kernel Modification

### 4.1 Direct Kernel Temperature Override

**File:** `runtime/metal/kernels/decode/sampling.metal`, line 1344

**Original:**
```metal
float probability = exp((scores[i] - maximum) / params.temperature[batch]);
```

**Modified:**
```metal
const float draft_temperature = 0.8f; // EXPERIMENT
float probability = exp((scores[i] - maximum) / draft_temperature);
```

**Pros:** Even simpler than C++ change (2 minutes to modify)  
**Cons:** Harder to find/document, bypasses parameter system  
**Use case:** Ultra-quick exploratory test before committing to POC

---

## 5. Effort Comparison

| Approach | Time | Flexibility | Completeness | Recommended |
|----------|------|-------------|--------------|-------------|
| Metal kernel hack | 2 min | One fixed value | Prototype only | No |
| C++ compile-time POC | 25 min | 5 values tested | Exploration | **Yes (do first)** |
| Full runtime flags | 1.5 days | Unlimited configs | Production-ready | **If POC succeeds** |
| Skip to Phase 2 | 0 | N/A | Addresses root cause | **If POC fails** |

---

## 6. Decision Matrix

### 6.1 When to Implement Full Runtime Flags

**Implement if:**
- ✅ POC shows ≥2 percentage point gain (22% → 24-26%)
- ✅ 1.5 days is available before Phase 2 starts
- ✅ Acceptance target is 24-28% (not 32-35%)

**Skip if:**
- ❌ POC shows <2 percentage point gain
- ❌ Goal is 32-35% acceptance (needs Phase 2 codebook retraining)
- ❌ Only 1-2 days available total (spend on Phase 2 planning instead)

### 6.2 Phase Comparison Updated

| Phase | Acceptance | Effort | Gated By | Status |
|-------|-----------|--------|----------|--------|
| Baseline | 22% | — | — | ✅ Current |
| **POC (compile-time)** | **24-26%?** | **25 min** | **This investigation** | ⏭️ **Recommended next** |
| Phase 1 (runtime flags) | 24-28% | 1.5 days | POC success | ⚠️ Conditional |
| Phase 2 (codebook retrain) | 32-35% | 3-4 weeks | — | ✅ Primary path |

---

## 7. Immediate Next Steps

### 7.1 This Week (Hours 0-2)

1. ✅ Complete this investigation (this document)
2. ⏭️ **Implement compile-time POC** (Section 3.1)
3. ⏭️ **Run 5 temperature experiments** (Section 3.2)
4. ⏭️ **Measure acceptance rates** (Section 3.2)

### 7.2 Decision Point (Hour 2)

**If POC acceptance ≥24%:**
- Proceed to full runtime implementation (Section 2)
- Estimated completion: 1.5 days from decision
- Target: 24-28% acceptance, ×2.7 speedup

**If POC acceptance <24%:**
- Skip Phase 1 entirely
- Proceed directly to Phase 2 (codebook retraining)
- Target: 32-35% acceptance, ×3.0 speedup

---

## 8. Files to Modify (Full Implementation)

| File | Lines | Purpose | Complexity |
|------|-------|---------|------------|
| `server/serve_options.py` | +10 | Add arg definitions | Low |
| `server/server.py` | +4 | Thread to native command | Low |
| `runtime/engine/NativeArguments.hpp` | +3 | Add fields | Low |
| `runtime/engine/NativeArguments.cpp` | +16 | Parse flags | Low |
| `runtime/model/DFlashDraft.hpp` | +2 | Store draft policy | Low |
| `runtime/model/DFlashDraft.cpp` | +15 | Construct draft policy | Medium |
| `runtime/model/Model.cpp` | +8 | Pass to DraftSelector | Medium |
| `runtime/ops/DraftSelector.cpp` | +5 | Use draft policy | Low |

**Total:** ~70 lines of code across 8 files

---

## 9. Validation Criteria

### 9.1 POC Success Metrics

- ✅ Acceptance rate increases ≥2 percentage points
- ✅ Decode throughput stable (±5%)
- ✅ No increase in TTFT
- ✅ Temperature 0.7-0.8 outperforms 1.0 (baseline)

### 9.2 Full Implementation Success Metrics

- ✅ `--draft-temperature=0.8` reproduces POC results
- ✅ Acceptance rate 24-28% sustained across workloads
- ✅ Default behavior (no flags) matches baseline 22%
- ✅ No memory leaks or crashes after 10K requests

---

## 10. Conclusion

**Phase 1 is feasible** with two paths:

1. **Quick POC (25 min):** Compile-time temperature override to measure impact
2. **Full implementation (1.5 days):** Runtime `--draft-temperature` flag

**Recommendation:**
1. **Execute POC first** (Section 3) to validate hypothesis
2. **If POC shows ≥2 point gain:** Proceed to full implementation (Section 2)
3. **If POC shows <2 point gain:** Skip to Phase 2 (codebook retraining)

**Key Finding:** Temperature mechanism already exists in the codebase (line 1344 of `sampling.metal`), it just needs to be decoupled from target temperature.

**Expected POC outcome:** 24-26% acceptance with draft temperature 0.7-0.8 (improvement of 2-4 percentage points).

---

**Next Action:** Implement compile-time POC and run 5 temperature experiments (~2 hours total).
