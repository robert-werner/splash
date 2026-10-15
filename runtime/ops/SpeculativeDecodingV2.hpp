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
  
  bool shouldAcceptDraft(float score) const;
  uint32_t getMaxDraftTokens() const;
  float getSpeedupFactor() const;

private:
  SpeculativeDecodingV2Config config_;
};

} // namespace splash::ops
