#pragma once

#include "metal/MetalBackend.hpp"
#include "ops/ExecutionPlans.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace splash::metal {

// Pipeline warming configuration: which pipeline families to warm at startup.
struct PipelineWarmingConfig {
  bool prefillAttention = true;
  bool decodeAttention = true;
  bool fusedKernels = true;
  bool draftModel = true;
  
  // KV formats to warm: int4, int8, bf16
  bool kvInt4 = true;
  bool kvInt8 = true;
  bool kvBf16 = true;
};

// Result of pipeline warming operation.
struct PipelineWarmingResult {
  uint32_t pipelinesWarmed = 0;
  double wallSeconds = 0.0;
  bool success = true;
  std::string errorMessage;
};

// Warm critical Metal compute pipelines at startup to avoid first-request
// compilation penalty. Compiles and caches pipelines without executing them.
// 
// This eliminates the 2-3s TTFT penalty on the first request by taking ~500ms
// at startup instead. The warming is synchronous and blocks the caller.
//
// Pipelines warmed:
// - Prefill attention (q4/q8/bf16 KV formats)
// - Decode attention (q4/q8/bf16 KV formats)
// - Fused RMSNorm+Linear kernels
// - Draft model decode kernels (for speculative execution)
//
// The backend must be initialized and the metallib loaded before calling this.
[[nodiscard]] PipelineWarmingResult
warmCriticalPipelines(MetalBackend &backend,
                      const ops::ExecutionPlans &plans,
                      const PipelineWarmingConfig &config = {});

} // namespace splash::metal
