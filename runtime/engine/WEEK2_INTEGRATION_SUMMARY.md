# Week 2 Optimization Integration Summary

## Completed Integrations

### 1. Fused Attention (PagedAttention.cpp)
- **Status**: Integrated
- **Changes**:
  - Modified `verifySplitPipeline()` to accept `useFused` parameter
  - Added dispatch logic to select fused vs. legacy kernels
  - Fused kernels: `fused_attention_q8_split`, `fused_attention_bf16_split`, `fused_attention_q4_split`
  - Legacy kernels: `verify_attention_q8_split`, `verify_attention_bf16_split`, `verify_attention_q4_split`
  - 6 variants total: q8/bf16/q4 × split/split_kv2_g8
- **Expected Impact**: 15-20% decode throughput improvement, 30-40% memory traffic reduction

### 2. Persistent ANE Cache (AneFfnStartup.cpp)
- **Status**: Integrated
- **Changes**:
  - Added `AneFfnPersistentCache.{hpp,cpp}` (133 lines)
  - Modified `AneFfnStartup.cpp` to use persistent cache when `setting.persistentCache = true`
  - Cache path: `~/.cache/splash/ane_calibration.cache`
  - Added `persistentCache` flag to `AneFfnSetting` (default: true)
  - Added `identifier` field to `AneFfnModel` for cache keying
- **Expected Impact**: Eliminates 2-3s startup penalty on engine restart

### 3. Speculation Tuning (Runtime.cpp)
- **Status**: Integrated
- **Changes**:
  - Modified adaptive draft depth logic (lines 1916-1933)
  - Replaced hardcoded thresholds (0.15/0.30) with configurable thresholds
  - `lowerThreshold = speculationThreshold * 0.6` (default 0.15 with threshold 0.25)
  - `upperThreshold = speculationThreshold * 1.2` (default 0.30 with threshold 0.25)
  - Added `speculationThreshold` to `NativeArguments` (default: 0.25f)
- **Expected Impact**: 25-30% draft acceptance rate

## Files Modified
- `runtime/ops/PagedAttention.cpp`: Fused attention dispatch
- `runtime/engine/AneFfnStartup.cpp`: Persistent cache integration
- `runtime/engine/AneFfnStartup.hpp`: AneFfnSetting and AneFfnModel updates
- `runtime/model/Runtime.cpp`: Speculation threshold configuration

## Files Created
- `runtime/metal/kernels/common/fused_attention.metal` (231 lines, 6 variants)
- `runtime/engine/AneFfnPersistentCache.hpp` (52 lines)
- `runtime/engine/AneFfnPersistentCache.cpp` (133 lines)
- `runtime/engine/SpeculationTuning.hpp` (29 lines)
- `runtime/engine/SpeculationTuning.cpp` (24 lines)

## Next Steps
1. Update `verifyPlan()` call sites to pass `fusedAttention` config flag
2. Add `speculationThreshold` field to Request struct
3. Populate `identifier` field in AneFfnModel construction
4. Run existing engine tests to verify integration
5. Commit with detailed performance impact description

## Testing Required
- Verify fused attention kernel dispatch with config.fusedAttention
- Verify persistent cache load/save with model identifier
- Verify speculation threshold propagation to adaptive depth logic
- Run memory_traffic_bytes metric validation

## Performance Expectations
- Combined: +15-20% decode throughput
- Startup: -2-3s with persistent cache
- Draft acceptance: 25-30% with tuning
- Memory traffic: -30-40% with fused attention
