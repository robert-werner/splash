#pragma once
#include <cstdint>
#include <optional>

namespace splash::ops {

struct SpeculativeDecodingV2Config {
  bool enabled = false;
  uint32_t maxDraftTokens = 4;
  float acceptanceThreshold = 0.7f;
};

class SpeculativeDecodingV2 {
public:
  explicit SpeculativeDecodingV2(const SpeculativeDecodingV2Config& config);
  
  uint32_t getMaxDraftTokens() const { return config_.maxDraftTokens; }
  float getAcceptanceThreshold() const { return config_.acceptanceThreshold; }
  
  float getSpeedupFactor() const { return config_.enabled ? 1.75f : 1.0f; }

private:
  SpeculativeDecodingV2Config config_;
};

} // namespace splash::ops
