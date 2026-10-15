#include "ops/SpeculativeDecodingV2.hpp"

namespace splash::ops {

SpeculativeDecodingV2::SpeculativeDecodingV2(const SpeculativeDecodingV2Config& config) : config_(config) {}

bool SpeculativeDecodingV2::shouldAcceptDraft(float score) const {
  return config_.enabled && score >= config_.acceptanceThreshold;
}

uint32_t SpeculativeDecodingV2::getMaxDraftTokens() const {
  return config_.enabled ? config_.maxDraftTokens : 0;
}

float SpeculativeDecodingV2::getSpeedupFactor() const {
  return config_.enabled ? 1.75f : 1.0f;
}

} // namespace splash::ops
