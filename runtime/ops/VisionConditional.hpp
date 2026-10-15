#pragma once
#include <cstdint>

namespace splash::ops {

struct VisionConditionalConfig {
  bool enabled = false;
  bool textOnlyMode = true;
};

class VisionConditional {
public:
  explicit VisionConditional(const VisionConditionalConfig& config);
  
  bool shouldLoadVisionLayers() const;
  uint32_t getMemorySavings() const;

private:
  VisionConditionalConfig config_;
};

} // namespace splash::ops
