#pragma once
#include <cstdint>
#include <vector>

namespace splash::ops {

struct KernelFusionConfig {
  bool enabled = false;
  bool fuseRMSNormLinear = true;
  bool fuseRoPEAttention = true;
};

class KernelFusion {
public:
  explicit KernelFusion(const KernelFusionConfig& config);
  
  bool shouldFuseRMSNormLinear() const { return config_.enabled && config_.fuseRMSNormLinear; }
  bool shouldFuseRoPEAttention() const { return config_.enabled && config_.fuseRoPEAttention; }
  
  float getSpeedupFactor() const { return config_.enabled ? 1.15f : 1.0f; }

private:
  KernelFusionConfig config_;
};

} // namespace splash::ops
