#include "engine/SpeculationTuning.hpp"
#include <algorithm>
#include <cmath>

namespace splash::engine {

SpeculationTuning SpeculationTuning::fromThreshold(float threshold) {
  SpeculationTuning tuning;
  tuning.acceptanceThreshold = threshold;
  tuning.validate();
  return tuning;
}

void SpeculationTuning::validate() {
  // Clamp threshold to valid range
  acceptanceThreshold = std::clamp(acceptanceThreshold, 0.0f, 1.0f);
  
  // Ensure threshold is finite
  if (!std::isfinite(acceptanceThreshold)) {
    acceptanceThreshold = 0.5f;
  }
}

} // namespace splash::engine
