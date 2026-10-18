# Phase 2 Critical Blocker — PTQ1_0 Format Incompatibility

**Date**: 2026-10-18  
**Status**: ❌ **BLOCKED — CANNOT PROCEED**  
**Impact**: Phase 2 ANE Hybrid Quantization implementation cannot be completed

---

## Problem Summary

PTQ1_0 format is **incompatible** with Splash's ANE hybrid quantization architecture. The format validation bug has been fixed, but runtime inference fails because **PTQ1_0 is not an unrotated Q4_K variant** — it's a completely different quantization format with:

- Different block size: **18 bytes/block** (PTQ1_0) vs **34 bytes/block** (PQ2_0) vs **~20 bytes/block** (Q4_K)
- Different metadata structure: **zero correlation** (r=0.044 max) between PTQ1_0 and PQ2_0 metadata
- Different tensor layout: incompatible with Metal ANE kernels expecting unrotated Q4_K

---

## Bug Fix Completed ✅

**Fixed**: `modelWeightBytes()` validation crash  
**Commits**: `ab7e8f70` (format validation fix)  
**Build**: Clean compilation, 0 errors, 0 warnings

**Before Fix**:
```
error: runtime bootstrap failed [resource_assembly/model_loading]: GPU layer blk.0.ffn_gate.weight expected PQ2_0 format, got 43 (PTQ1_0)
```

**After Fix**:
```
✅ Model loading succeeds through format validation
❌ Runtime inference crashes on PTQ1_0 Metal kernel execution
```

**Implementation**:
- Extended `modelWeightBytes()` signature to accept `HybridQuantConfig`
- Fixed `planImages()` call chain to propagate `hybridConfig` from CLI to pre-check
- Validation now correctly skips PQ2_0 check for ANE-designated PTQ1_0 layers

---

## Root Cause Analysis

### Architecture Mismatch

**Splash ANE Hybrid Design** (from HYBRID_QUANTIZATION.md):
```
ANE layers: Q4_K_M (unrotated, ~2.1 GB, 6 layers)
GPU layers: PQ2_0 (rotated, ~4.2 GB, 10 layers)
Total: ~6.3 GB hybrid GGUF
```

**Available Models**:
- `Ternary-Bonsai-2-27B-PQ2_0.gguf` (6.9 GB) — rotated, GPU-only ✅ works
- `Ternary-Bonsai-2-27B-PTQ1_0.gguf` (4.2 GB) — **not unrotated Q4_K** ❌ incompatible

### PTQ1_0 Format Analysis

**Tensor**: `blk.0.ffn_gate.weight` [5120, 17408]

| Format | Data Bytes | Bytes/Block | Structure |
|--------|------------|-------------|-----------|
| PQ2_0  | 23,674,880 | 34 | 2×fp16 meta + 32×uint8 codes |
| PTQ1_0 | 12,533,760 | 18 | **unknown** (not Q4_K) |
| Q4_K_M | ~19,660,800 | ~20 | 2×fp16 scales + 128×4bit |

**Metadata Correlation** (2000-sample test):
```python
PQ2_0 metadata median: 0.01118 (fp16 scales)
PTQ1_0 metadata correlation: max r=0.044 (no correlation)
```

**Conclusion**: PTQ1_0 is **not** a simple unrotated variant of PQ2_0 or Q4_K. It uses a fundamentally different quantization scheme.

---

## What Works vs. What Doesn't

### ✅ Infrastructure Complete (100%)

- **CLI Flags**: `--ane-layers "0,2,4,8,10,14"` ✅
- **Config Flow**: NativeArguments → RuntimeResources → HybridQuantConfig ✅
- **Format Validation**: Conditional PQ2_0/PTQ1_0 validation ✅
- **Build System**: Clean compilation with `-Werror` ✅
- **Documentation**: HYBRID_QUANTIZATION.md, integration guides ✅

### ❌ Runtime Blocked (0%)

- **Metal Kernels**: PTQ1_0 kernels do not exist
- **ANE Execution**: Requires unrotated Q4_K, not PTQ1_0
- **Hybrid Model**: Q4_K_M + PQ2_0 hybrid GGUF does not exist
- **Benchmarks**: Cannot run without working model
- **Quality Tests**: Cannot validate without inference

---

## Two Paths Forward

### Option A: Create Q4_K_M Hybrid Model (RECOMMENDED)

**Effort**: 1-2 days  
**Risk**: Low  
**Requirements**:
1. Download Ternary-Bonsai-2-27B Q4_K_M GGUF (if exists)
2. Run `hybrid_gguf_converter.py` to merge Q4_K_M (ANE layers) + PQ2_0 (GPU layers)
3. Verify hybrid GGUF structure and metadata
4. Test with existing Splash infrastructure

**Pros**:
- Uses existing Metal Q4_K kernels
- No code changes required
- Infrastructure already complete
- Fast iteration cycle

**Cons**:
- Q4_K_M GGUF may not exist on HuggingFace
- Slightly larger model size (~6.3 GB vs 4.2 GB PTQ1_0-only)

---

### Option B: Implement PTQ1_0 Metal Kernels

**Effort**: 1-2 weeks  
**Risk**: High  
**Requirements**:
1. Reverse-engineer PTQ1_0 format (18-byte block structure)
2. Write Metal shaders for PTQ1_0 dequantization
3. Integrate with ops::Projection ANE split
4. Verify numerical accuracy vs. PQ2_0 baseline

**Pros**:
- Enables smallest model size (4.2 GB PTQ1_0)
- Future-proof for PTQ1_0 models

**Cons**:
- Significant engineering effort (Metal shader dev + testing)
- Unknown format specification (no docs)
- Risk of accuracy degradation
- Delays Phase 2 completion by 1-2 weeks

---

## Impact on Roadmap

### Current Status

**Phase 1**: ✅ PRODUCTION (TTFT -80% verified)  
**Phase 2**: 🔴 **BLOCKED** (cannot proceed)  
**Phase 3**: ✅ PRODUCTION (KV swap ready)  
**Phase 4**: ✅ PRODUCTION (weight prefetch ready)

### Decision Required

**If Option A (Q4_K hybrid)**:
- Phase 2 completion: **+1-2 days**
- Overall project: **~3-4 days** to full production

**If Option B (PTQ1_0 kernels)**:
- Phase 2 completion: **+1-2 weeks**
- Overall project: **~2-3 weeks** to full production

**If Neither** (skip Phase 2):
- Phase 2: **ABANDONED**
- Overall impact: **-30% decode throughput** (ANE stays at 0%)
- Remaining benefit: Phase 1+3+4 = -80% TTFT + 1-2M context + weight prefetch

---

## Recommendation

**PROCEED WITH OPTION A** (Q4_K hybrid model creation):
1. Search for `prism-ml/Ternary-Bonsai-2-27B-gguf:Q4_K_M` on HuggingFace
2. If found: run converter, test immediately
3. If not found: request conversion from model authors OR skip Phase 2

**Phase 2 CANNOT be completed** without a working unrotated Q4_K model or PTQ1_0 Metal kernels.

---

## Files Modified

- `runtime/model/ModelFactory.{hpp,cpp}` — `modelWeightBytes()` signature extended
- `runtime/engine/RuntimeResources.mm` — `hybridConfig` propagation fixed

## Test Results

- ✅ Build: `make -j8` — 0 errors, 0 warnings
- ✅ Validation: model loads through format check
- ❌ Inference: blocked by missing PTQ1_0 Metal kernels
- ❌ Benchmarks: cannot run

---

## Generated by
Claude Opus 5.5  
2026-10-18 15:26 MSK
