> SUPERSEDED by STATUS.md — it tracks ANE hybrid as "compilation fixed, integration incomplete" toward +30% decode on a 62.9 tok/s baseline; the integration could never complete, because the split requires unrotated weights this checkpoint does not have

# Phase 2 ANE Hybrid Quantization - Status

**Commit:** a21d29a0  
**Date:** 2026-10-18  
**Status:** ✅ Compilation Fixed, ⚠️ Integration Incomplete

---

## ✅ Completed (this session)

### 1. Fixed Compilation Errors
- ✅ Added `std::vector<uint32_t> aneLayerIndices` to `NativeArguments.hpp`
- ✅ Added CLI parsing for `--ane-layers "0,2,4,8,10,14"` in `NativeArguments.cpp`
- ✅ Added `std::vector<uint32_t> aneLayerIndices` to `RuntimeResourcesConfig` in `RuntimeResources.hpp`
- ✅ Wired field through `main.mm` → `RuntimeResourcesConfig` → `RuntimeResources::create()`
- ✅ Added Python CLI support in `server/serve_options.py` and `server/server.py`
- ✅ Clean build: `make clean && make -j8` → **0 errors, 0 warnings**

### 2. Infrastructure Ready (from commit 203d730a)
- ✅ `HybridQuantization.hpp` and `HybridQuantization.cpp` classes
- ✅ `HybridQuantizationConfig` with `isAneLayer()` and `isGpuLayer()` methods
- ✅ Unit tests for layer classification

---

## ⚠️ Remaining Integration Work

### 1. GgufTargetLoader Integration
**File:** `runtime/model/GgufTargetLoader.cpp`  
**Task:** Per-layer quantization loading logic

```cpp
// In loadFfnLayer() or equivalent:
if (hybridConfig.isAneLayer(layerIndex)) {
    // Load Q4_K weights WITHOUT rotation
    // ANE requires unrotated weights for direct computation
} else {
    // Load PQ2_0 weights WITH rotation
    // GPU uses rotated PQ2_0 for optimal memory layout
}
```

**Impact:** Without this, all layers will load with the same quantization format.

---

### 2. AneFfn.cpp Integration
**File:** `runtime/ops/AneFfn.cpp`  
**Task:** Selective ANE initialization

```cpp
// In constructor or initialization:
for (uint32_t layerIdx = 0; layerIdx < numLayers; ++layerIdx) {
    if (hybridConfig.isAneLayer(layerIdx)) {
        // Create ANE plane views for this layer
        // Initialize ANE resources
    } else {
        // Skip ANE initialization - this layer runs on GPU
    }
}
```

**Impact:** Without this, ANE will attempt to initialize all layers or none.

---

### 3. AneFfnStartup.cpp Integration
**File:** `runtime/engine/AneFfnStartup.cpp`  
**Task:** Selective calibration

```cpp
// In calibration loop:
for (uint32_t layerIdx = 0; layerIdx < numLayers; ++layerIdx) {
    if (hybridConfig.isAneLayer(layerIdx)) {
        // Run ANE calibration for this layer
    } else {
        // Skip calibration - GPU layer
    }
}
```

**Impact:** Calibration will waste time on GPU-only layers.

---

## 🧪 Testing Plan

### 1. Compilation Test ✅
```bash
make clean && make -j8
# Result: 0 errors, 0 warnings
```

### 2. Runtime Test (TODO)
**Model:** `prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0`  
**Command:**
```bash
python -m splash.server serve \
  prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0 \
  --ane-layers "0,2,4,8,10,14" \
  --max-context auto
```

**Expected Behavior:**
- Startup logs show hybrid quantization config
- 6 FFN layers on ANE (Q4_K, ~2.1 GB)
- 10 FFN layers on GPU (PQ2_0, ~4.2 GB)
- ANE utilization: 35-40% (Activity Monitor)
- Throughput: 62.9 → 81.8 tok/s (+30%)
- Context: 262K → 200K (acceptable trade-off)

---

## 📊 Architecture Summary

### Hybrid Quantization Strategy
- **ANE layers (6):** Indices 0, 2, 4, 8, 10, 14
  - Format: Q4_K unrotated
  - Memory: ~2.1 GB
  - Benefit: High-throughput matrix multiply on Neural Engine
  
- **GPU layers (10):** Remaining indices
  - Format: PQ2_0 rotated
  - Memory: ~4.2 GB
  - Benefit: Low-memory quantization with SIMD acceleration

### Trade-offs
- ✅ **+30% throughput** (62.9 → 81.8 tok/s)
- ✅ **ANE utilization** rises to 35-40% (from 0%)
- ⚠️ **Context reduction** 262K → 200K (24% decrease)
- ⚠️ **Memory increase** ~2.1 GB (Q4_K) vs PQ2_0 baseline

---

## 🔧 Implementation Notes

### Current State
1. ✅ CLI accepts `--ane-layers "0,2,4,8,10,14"`
2. ✅ Parsed into `std::vector<uint32_t> aneLayerIndices`
3. ✅ Passed through to `RuntimeResourcesConfig`
4. ✅ Available in `RuntimeResources::create()`
5. ⚠️ **NOT YET USED** by loader/ANE initialization

### Next Steps (Priority Order)
1. **GgufTargetLoader.cpp:** Conditional per-layer quantization loading
2. **AneFfn.cpp:** Selective ANE plane view creation
3. **AneFfnStartup.cpp:** Selective calibration
4. **Test:** Run with Bonsai-2-27B and measure throughput

---

## 📝 Commit History

- **203d730a:** Initial HybridQuantization infrastructure
- **a21d29a0:** Fix compilation errors, add CLI support (this commit)
- **TODO:** Complete GgufTargetLoader integration
- **TODO:** Complete AneFfn integration
- **TODO:** Complete AneFfnStartup integration

---

## 🚀 Performance Target

| Metric | Baseline (PQ2_0) | Target (Hybrid) | Status |
|--------|------------------|-----------------|--------|
| Throughput | 62.9 tok/s | 81.8 tok/s (+30%) | ⏳ Pending test |
| ANE Utilization | 0% | 35-40% | ⏳ Pending test |
| Context | 262K | 200K (-24%) | ⏳ Pending test |
| Memory | ~4.2 GB | ~6.3 GB | ⏳ Pending test |

---

**Next Subagent Task:** Complete GgufTargetLoader, AneFfn, and AneFfnStartup integrations
