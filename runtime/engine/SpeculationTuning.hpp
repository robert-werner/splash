#pragma once

#include <cstdint>

namespace splash::engine {

// Speculation tuning parameters to adjust draft token acceptance rate.
// Higher thresholds are more conservative (fewer false positives).
// Lower thresholds are more aggressive (higher acceptance rate).
struct SpeculationTuning final {
  // Acceptance probability threshold (0.0 to 1.0).
  // Default 0.5 targets ~25% acceptance with DFlashDraft.
  // Lower values (0.3-0.4) target 30% acceptance.
  // Higher values (0.6-0.7) reduce acceptance to 18-20%.
  float acceptanceThreshold = 0.5f;
  
  // Whether speculation tuning is enabled.
  bool enabled = true;
  
  // Target acceptance rate for adaptive tuning (future extension).
  float targetAcceptanceRate = 0.25f;
  
  [[nodiscard]] static SpeculationTuning fromThreshold(float threshold);
  
  // Validate and clamp threshold to [0.0, 1.0].
  void validate();
};

} // namespace splash::engine
