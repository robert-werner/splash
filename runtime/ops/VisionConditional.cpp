#include "ops/VisionConditional.hpp"

namespace splash::ops {

VisionConditional::VisionConditional(const VisionConditionalConfig& config) : config_(config) {}

bool VisionConditional::shouldLoadVisionLayers() const {
  return config_.enabled && !config_.textOnlyMode;
}

uint32_t VisionConditional::getMemorySavings() const {
  return config_.textOnlyMode ? 30000 : 0;
}

} // namespace splash::ops
