#include "metal/PipelineWarming.hpp"
#include "AwakeClock.hpp"
#include "StderrLine.hpp"

#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <vector>

namespace splash::metal {
namespace {

// Representative dispatch configurations for critical pipelines.
// These match the actual shapes used during inference but don't allocate
// real buffers or execute - just compile the pipelines.

std::vector<ComputeDispatch> prefillAttentionDispatches(
    const ops::ExecutionPlans & /*plans*/, bool int4, bool int8, bool bf16) {
  std::vector<ComputeDispatch> dispatches;
  
  // Prefill attention for common batch widths (1, 2, 4) and token counts
  // These pipeline names must match what ExecutionPlans actually uses
  const std::vector<std::string_view> kvFormats;
  if (int4) {
    dispatches.push_back({
        .pipelineName = "prefill_attention_q4",
        .threadgroups = {32, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  if (int8) {
    dispatches.push_back({
        .pipelineName = "prefill_attention_q8",
        .threadgroups = {32, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  if (bf16) {
    dispatches.push_back({
        .pipelineName = "prefill_attention_bf16",
        .threadgroups = {32, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  
  return dispatches;
}

std::vector<ComputeDispatch> decodeAttentionDispatches(
    const ops::ExecutionPlans & /*plans*/, bool int4, bool int8, bool bf16) {
  std::vector<ComputeDispatch> dispatches;
  
  // Decode attention for common batch widths
  if (int4) {
    dispatches.push_back({
        .pipelineName = "decode_attention_q4",
        .threadgroups = {16, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  if (int8) {
    dispatches.push_back({
        .pipelineName = "decode_attention_q8",
        .threadgroups = {16, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  if (bf16) {
    dispatches.push_back({
        .pipelineName = "decode_attention_bf16",
        .threadgroups = {16, 1, 1},
        .threadsPerThreadgroup = {32, 1, 1},
    });
  }
  
  return dispatches;
}

std::vector<ComputeDispatch> fusedKernelDispatches(
    const ops::ExecutionPlans & /*plans*/) {
  std::vector<ComputeDispatch> dispatches;
  
  // Fused RMSNorm+Linear decode kernel
  dispatches.push_back({
      .pipelineName = "norm_rms_table16_decode",
      .threadgroups = {16, 1, 1},
      .threadsPerThreadgroup = {32, 1, 1},
  });
  
  return dispatches;
}

std::vector<ComputeDispatch> draftModelDispatches(
    const ops::ExecutionPlans & /*plans*/) {
  std::vector<ComputeDispatch> dispatches;
  
  // Draft model decode kernels for speculative execution
  dispatches.push_back({
      .pipelineName = "draft_decode_attention",
      .threadgroups = {8, 1, 1},
      .threadsPerThreadgroup = {32, 1, 1},
  });
  
  dispatches.push_back({
      .pipelineName = "draft_decode_mlp",
      .threadgroups = {8, 1, 1},
      .threadsPerThreadgroup = {32, 1, 1},
  });
  
  return dispatches;
}

} // namespace

PipelineWarmingResult warmCriticalPipelines(
    MetalBackend &backend, const ops::ExecutionPlans &plans,
    const PipelineWarmingConfig &config) {
  PipelineWarmingResult result;
  
  try {
    const auto started = AwakeClock::now();
    
    std::vector<ComputeDispatch> allDispatches;
    
    // Collect all pipelines to warm based on configuration
    if (config.prefillAttention) {
      auto dispatches = prefillAttentionDispatches(
          plans, config.kvInt4, config.kvInt8, config.kvBf16);
      allDispatches.insert(allDispatches.end(), dispatches.begin(), dispatches.end());
    }
    
    if (config.decodeAttention) {
      auto dispatches = decodeAttentionDispatches(
          plans, config.kvInt4, config.kvInt8, config.kvBf16);
      allDispatches.insert(allDispatches.end(), dispatches.begin(), dispatches.end());
    }
    
    if (config.fusedKernels) {
      auto dispatches = fusedKernelDispatches(plans);
      allDispatches.insert(allDispatches.end(), dispatches.begin(), dispatches.end());
    }
    
    if (config.draftModel) {
      auto dispatches = draftModelDispatches(plans);
      allDispatches.insert(allDispatches.end(), dispatches.begin(), dispatches.end());
    }
    
    if (allDispatches.empty()) {
      result.success = true;
      result.pipelinesWarmed = 0;
      result.wallSeconds = 0.0;
      return result;
    }
    
    // Prepare (compile) all pipelines without executing them
    // preparePipelines validates and compiles but doesn't encode or commit
    backend.preparePipelines(allDispatches);
    
    const std::chrono::duration<double> elapsed = AwakeClock::now() - started;
    
    result.success = true;
    result.pipelinesWarmed = static_cast<uint32_t>(allDispatches.size());
    result.wallSeconds = elapsed.count();
    
    logLine("Pipeline warming: compiled ", result.pipelinesWarmed,
            " critical pipelines in ", std::fixed, std::setprecision(3),
            result.wallSeconds, " s");
    
  } catch (const std::exception &error) {
    result.success = false;
    result.errorMessage = error.what();
    logLine("Pipeline warming failed: ", error.what());
  }
  
  return result;
}

} // namespace splash::metal
