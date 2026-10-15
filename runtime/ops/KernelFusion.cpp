#include "ops/KernelFusion.hpp"

namespace splash::ops {

KernelFusion::KernelFusion(const KernelFusionConfig& config) : config_(config) {}

bool KernelFusion::shouldFuseRMSNormLinear() const {
  return config_.enabled && config_.fuseRMSNormLinear;
}

bool KernelFusion::shouldFuseRoPEAttention() const {
  return config_.enabled && config_.fuseRoPEAttention;
}

float KernelFusion::getSpeedupFactor() const {
  if (!config_.enabled) return 1.0f;
  float speedup = 1.0f;
  if (config_.fuseRMSNormLinear) speedup *= 1.08f;
  if (config_.fuseRoPEAttention) speedup *= 1.07f;
  return speedup;
}

} // namespace splash::ops
