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
  
  bool shouldFuseRMSNormLinear() const;
  bool shouldFuseRoPEAttention() const;
  float getSpeedupFactor() const;

private:
  KernelFusionConfig config_;
};

} // namespace splash::ops
