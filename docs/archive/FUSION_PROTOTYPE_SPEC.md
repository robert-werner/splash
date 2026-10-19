> SUPERSEDED by STATUS.md — it specs a kernel-fusion prototype as "ready for implementation" with a 3-5% bandwidth gain; both fusions it describes are already on and correct in the normal path, and the --kernel-fusion flag was removed because its own kernel computed the wrong expression

# Kernel Fusion Prototype: Attention Reduce + RMSNorm

**Status**: Ready for implementation  
**Estimated Effort**: 2-3 days  
**Expected Gain**: 3-5% memory bandwidth reduction  
**Risk Level**: Low (modular, can be feature-flagged)

---

## Motivation

Current pipeline writes attention output to device memory, then immediately reads it for RMSNorm:

```
Attention Reduce → [DRAM write, 4 KB/tile]
                       ↓
                   [DRAM read, 4 KB/tile]
                       ↓
RMSNorm + Scale → FFN Input
```

**Fused approach** eliminates the intermediate roundtrip:

```
Attention Reduce → [registers] → RMSNorm → [DRAM write, 4 KB/tile]
```

**Bandwidth savings**: 8 KB/tile × 8192 tiles/batch = **64 MB per layer** (prefill 2048 tokens).

---

## Implementation Plan

### Phase 1: Prototype Kernel (Day 1)

**File**: `runtime/metal/kernels/ops/fused_attention_reduce_norm.metal`

**Based on**: `runtime/metal/kernels/common/paged_attention_tile.h::splash_attention_reduce_row`

**Additions**:
1. Inline RMSNorm computation after reduce accumulation
2. Load norm weights from buffer
3. Apply scale: `output = reduced * rsqrt(sum_sq/D + eps) * weight`

**Register budget**:
- Existing reduce: ~12 registers (estimate from MPP matmul)
- RMSNorm add: +4 registers (sq_sum, rms_inv, scaled, final)
- **Total**: ~16 registers **< 31 limit ✓**

**Threadgroup memory**:
- Existing: `float weights[splits]`, `float group_values[8]`
- New: `float rms_sums[8]` (per simdgroup reduction)
- **Total**: +32 bytes (negligible)

### Phase 2: Integration (Day 2)

**Files to modify**:

1. **runtime/ops/PagedAttention.cpp**:
   ```cpp
   void PrefillAttentionPlan::executeReduce(...) {
       if (use_fused_norm && norm_weights) {
           // Bind fused kernel
           encoder.setKernel("prefill_attention_reduce_norm");
           encoder.setBuffer(norm_weights, 3);
       } else {
           // Original separate kernels
           encoder.setKernel("prefill_attention_reduce");
       }
   }
   ```

2. **Feature flag** (config or environment variable):
   ```cpp
   bool fusion_enabled = getenv("SPLASH_FUSED_REDUCE_NORM") != nullptr;
   ```

3. **Fallback**: Always compile both kernels, runtime switch.

### Phase 3: Validation (Day 2-3)

**Correctness tests**:
```bash
# Compare outputs: fused vs baseline
./test_fused_reduce_norm --tolerance=1e-4
```

**Metal GPU trace**:
```bash
xcrun xctrace record \
    --template "Metal System Trace" \
    --output fused_reduce.trace \
    --launch ./splash eval --model qwen-7b --tokens 2048
```

**Metrics to extract**:
- Memory bandwidth (GB/s): look for reduction in write traffic
- Kernel duration (μs): should be ≤ sum of separate kernels
- Occupancy: should remain ≥ 80%

**Success criteria**:
- ✅ Numerical accuracy: max error < 1e-4 bf16
- ✅ Bandwidth reduction ≥ 3% (measured via Metal trace)
- ✅ Latency reduction ≥ 20 μs per layer
- ✅ No register spill (check via Xcode GPU debugger)

---

## Code Sketch

### Fused Kernel

```metal
// runtime/metal/kernels/ops/fused_attention_reduce_norm.metal
#include "metal/kernels/common/paged_attention_tile.h"

template <uint QueryHeadsPerKVHead, uint RowsPerTile>
inline void fused_reduce_norm_row(
    device const float *partials,
    device const float *statistics,
    device bfloat *tile_output,
    device const bfloat *norm_weight,
    float norm_epsilon,
    uint committed_tokens, uint active_rows, uint splits,
    ulong head_slot, uint fused_row, uint thread_index,
    threadgroup float *weights,
    threadgroup float *group_values,
    threadgroup float *rms_sums) {
    
    constexpr uint M = RowsPerTile * QueryHeadsPerKVHead;
    constexpr uint D = SplashKvHeadDimension;
    
    // Early exit for inactive rows
    if (fused_row / QueryHeadsPerKVHead >= active_rows) {
        tile_output[fused_row * D + thread_index] = bfloat(0.0f);
        return;
    }
    
    // === Phase 1: Attention Reduce (existing logic) ===
    const uint pages = splash_attention_pages(committed_tokens + active_rows);
    const uint per_split = splash_attention_pages_per_split(pages, splits);
    const uint written = (pages + per_split - 1) / per_split;
    
    // Compute split weights (reuse existing pattern)
    const uint lane = thread_index % 32, sg = thread_index / 32;
    const ulong stat = ((head_slot + thread_index) * M + fused_row) * 2;
    const float maximum = thread_index < written ? statistics[stat] : -INFINITY;
    const float group_maximum = simd_max(maximum);
    if (lane == 0) group_values[sg] = group_maximum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    
    const float row_maximum = simd_max(lane < 8 ? group_values[lane] : -INFINITY);
    if (thread_index < written)
        weights[thread_index] = fast::exp(maximum - row_maximum);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    
    // Accumulate partials
    float numerator = 0.0f, denominator = 0.0f;
    for (uint split = 0; split < written; ++split) {
        const float weight = weights[split];
        const ulong stat = ((head_slot + split) * M + fused_row) * 2;
        numerator += weight * partials[((head_slot + split) * M + fused_row) * D + thread_index];
        denominator += weight * statistics[stat + 1];
    }
    
    float reduced = denominator > 0.0f ? numerator / denominator : 0.0f;
    
    // === Phase 2: RMSNorm (NEW) ===
    float sq_sum = reduced * reduced;
    
    // Simdgroup reduction
    sq_sum = simd_sum(sq_sum);
    if (lane == 0) rms_sums[sg] = sq_sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    
    // Final reduction across simdgroups
    float total_sq = 0.0f;
    for (uint i = 0; i < 8; ++i) total_sq += rms_sums[i];
    
    float rms_inv = rsqrt(total_sq / float(D) + norm_epsilon);
    float normalized = reduced * rms_inv * float(norm_weight[thread_index]);
    
    // === Phase 3: Write output (single write) ===
    tile_output[fused_row * D + thread_index] = bfloat(normalized);
}

// Kernel entry point
#define FUSED_REDUCE_NORM(Name, Heads, Group) \
  kernel void Name( \
      device const float *partials [[buffer(0)]], \
      device const float *statistics [[buffer(1)]], \
      device bfloat *output [[buffer(2)]], \
      device const bfloat *norm_weight [[buffer(3)]], \
      constant SplashPrefillAttentionParams &params [[buffer(4)]], \
      uint3 group [[threadgroup_position_in_grid]], \
      uint thread_index [[thread_index_in_threadgroup]]) { \
    threadgroup float weights[SplashPrefillMaximumSplits]; \
    threadgroup float group_values[8]; \
    threadgroup float rms_sums[8]; \
    fused_reduce_norm_row<Heads, Group>( \
        partials, statistics, output, norm_weight, float(SPLASH_RMS_EPSILON), \
        params.committed_tokens + (group.z * SPLASH_PREFILL_ATTENTION_TILE_ROWS), \
        min(SPLASH_PREFILL_ATTENTION_TILE_ROWS, params.rows - group.z * SPLASH_PREFILL_ATTENTION_TILE_ROWS), \
        params.split_count, \
        (ulong(group.z) * /* KVHeads */ + group.x) * params.split_count, \
        group.y, thread_index, weights, group_values, rms_sums); \
  }

FUSED_REDUCE_NORM(prefill_attention_reduce_norm, 4, 6)
FUSED_REDUCE_NORM(prefill_attention_reduce_norm_kv2_g8, 2, 8)
```

### Host Integration

```cpp
// runtime/ops/PagedAttention.cpp

void PrefillAttentionPlan::executeReduceNorm(
    metal::CommandEncoder &encoder,
    const AttentionWorkspace &workspace,
    metal::MetalBuffer norm_weights,
    bool use_fused) {
    
    if (use_fused) {
        // Fused kernel
        encoder.setComputePipelineState(pipeline_fused_reduce_norm_);
        encoder.setBuffer(workspace.partials, 0);
        encoder.setBuffer(workspace.statistics, 1);
        encoder.setBuffer(workspace.output, 2);
        encoder.setBuffer(norm_weights, 3);  // NEW
        encoder.setBytes(&params_, sizeof(params_), 4);
        
        encoder.dispatchThreadgroups(
            MTL::Size(kv_heads, fused_rows, tiles),
            MTL::Size(256, 1, 1));
    } else {
        // Original separate path
        executeReduce(encoder, workspace);
        executeRMSNorm(encoder, workspace.output, norm_weights);
    }
}
```

---

## Testing Strategy

### Unit Test

```cpp
// dev/tests/engine/fused_reduce_norm_metal_test.mm

TEST(FusedReduceNorm, MatchesBaseline) {
    // Setup: create random partials + statistics
    auto partials = randomBuffer(splits * rows * dim * sizeof(float));
    auto statistics = randomBuffer(splits * rows * 2 * sizeof(float));
    auto norm_weights = randomBuffer(dim * sizeof(bfloat));
    
    // Run baseline (separate kernels)
    auto output_baseline = runBaselineReduce(partials, statistics);
    output_baseline = runRMSNorm(output_baseline, norm_weights);
    
    // Run fused kernel
    auto output_fused = runFusedReduceNorm(partials, statistics, norm_weights);
    
    // Compare
    float max_error = computeMaxError(output_baseline, output_fused);
    EXPECT_LT(max_error, 1e-4f);  // bf16 tolerance
}

TEST(FusedReduceNorm, RegistersUnderLimit) {
    // Requires: build with -emit-llvm, inspect register allocation
    // Or: run Xcode GPU debugger, check for spills
    // This is manual validation step
}
```

### Integration Test

```python
# test_fused_reduce_norm.py

def test_end_to_end_accuracy():
    """Run full forward pass with fused kernel, compare to baseline."""
    
    # Baseline
    output_baseline = run_splash([
        'eval', '--model', 'qwen-7b',
        '--prompt', 'test' * 256,
        '--feature', 'fused_reduce_norm=0'
    ])
    
    # Fused
    output_fused = run_splash([
        'eval', '--model', 'qwen-7b',
        '--prompt', 'test' * 256,
        '--feature', 'fused_reduce_norm=1'
    ])
    
    # Compare logits
    assert np.allclose(output_baseline, output_fused, rtol=1e-3, atol=1e-4)

def test_performance_improvement():
    """Measure latency reduction."""
    
    baseline_time = benchmark_splash(['--feature', 'fused_reduce_norm=0'])
    fused_time = benchmark_splash(['--feature', 'fused_reduce_norm=1'])
    
    improvement = (baseline_time - fused_time) / baseline_time
    print(f"Latency improvement: {improvement*100:.1f}%")
    assert improvement > 0.01  # At least 1% faster
```

---

## Rollout Plan

### Stage 1: Feature Flag (Week 1)
- Implement fused kernel
- Add `SPLASH_FUSED_REDUCE_NORM` environment variable
- Default: **disabled** (safe fallback)

### Stage 2: Validation (Week 2)
- Run correctness tests across all model configs
- Metal GPU trace analysis
- Collect performance data

### Stage 3: Gradual Rollout (Week 3-4)
- Enable for prefill only (decode path separate)
- Monitor production metrics (if applicable)
- Document performance characteristics

### Stage 4: Default Enable (Week 5+)
- If stable and beneficial: flip default to **enabled**
- Keep flag for debugging/regression testing
- Update documentation

---

## Risk Mitigation

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Register spill | Low | High | Pre-validate with Xcode profiler |
| Numerical instability | Low | Medium | Extensive unit tests, tolerance checks |
| Performance regression | Medium | Medium | Feature flag, easy rollback |
| Maintenance burden | Low | Low | Well-documented, isolated changes |

---

## Open Questions

- [ ] Does RMSNorm already have special-case optimizations we're duplicating?
- [ ] Should we fuse for decode path too? (smaller benefit, 8 rows only)
- [ ] Can we extend to other norm types (LayerNorm, GroupNorm)?
- [ ] Impact on speculative decoding path?

---

## Success Metrics

**After 1 month in production**:
- Memory bandwidth reduction: ≥ 3% (measured via Metal trace)
- Latency improvement: ≥ 1% (end-to-end prefill time)
- Zero correctness regressions (model output unchanged within tolerance)
- Zero stability issues (no crashes, no Metal validation errors)

---

**Owner**: TBD  
**Reviewer**: TBD  
**Target Completion**: TBD  

---

**References**:
- Main research: `/Users/a1111/KERNEL_FUSION_RESEARCH.md`
- Existing fusion example: `runtime/metal/kernels/ops/fused_rope_attention.metal`
- Attention reduce: `runtime/metal/kernels/common/paged_attention_tile.h:330`
