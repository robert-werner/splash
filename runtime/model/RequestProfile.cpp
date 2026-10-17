#include "RequestProfile.hpp"

#include <algorithm>
#include <cstring>

namespace splash::model {

namespace {

// Token ID ranges for classification heuristics (Qwen tokenizer patterns).
// These are approximate and model-specific; tune per tokenizer.
constexpr uint32_t kCodeTokenMin = 500;    // `def`, `class`, `function`, `{`, `}`
constexpr uint32_t kCodeTokenMax = 1500;
constexpr uint32_t kCreativeTokenMin = 8000; // Narrative, expressive vocab
constexpr uint32_t kCreativeTokenMax = 20000;

// Keyword-based heuristics: presence signals request type.
bool hasCodePatterns(std::span<const uint32_t> tokens) noexcept {
  uint32_t codeCount = 0;
  for (uint32_t token : tokens) {
    if (token >= kCodeTokenMin && token <= kCodeTokenMax)
      ++codeCount;
  }
  return codeCount >= tokens.size() / 6; // 16%+ code tokens → Code type
}

bool hasCreativePatterns(std::span<const uint32_t> tokens) noexcept {
  uint32_t creativeCount = 0;
  for (uint32_t token : tokens) {
    if (token >= kCreativeTokenMin && token <= kCreativeTokenMax)
      ++creativeCount;
  }
  return creativeCount >= tokens.size() / 4; // 25%+ creative vocab → Creative
}

// Translation heuristic: multiple language scripts or translation markers.
bool hasTranslationPatterns(std::span<const uint32_t> tokens) noexcept {
  // Simplified: check for typical translation prompt structure.
  // Real implementation would detect language script mixing or "translate" keyword.
  return tokens.size() > 10 && tokens[0] < 100 && tokens[1] > 30000;
}

} // namespace

RequestType RequestProfile::classifyRequest(
    std::span<const uint32_t> promptTokens) noexcept {
  if (promptTokens.empty())
    return RequestType::Unknown;
  
  // Sample first 50 tokens for classification; full prompt is wasteful.
  const auto sample = promptTokens.subspan(
      0, std::min<size_t>(50, promptTokens.size()));
  
  // Priority order: Code > Translation > Creative > Factual (fallback).
  if (hasCodePatterns(sample))
    return RequestType::Code;
  if (hasTranslationPatterns(sample))
    return RequestType::Translation;
  if (hasCreativePatterns(sample))
    return RequestType::Creative;
  
  // Default: Factual (QA, summaries, most chat).
  return RequestType::Factual;
}

uint32_t RequestProfile::optimalDraftDepth(RequestType type) const noexcept {
  const TypeHistory& history = historyFor(type);
  const float rate = history.acceptanceRate();
  
  // Base draft depth per type (empirical starting points).
  uint32_t base = 4;
  switch (type) {
    case RequestType::Code:        base = 7; break;
    case RequestType::Creative:    base = 3; break;
    case RequestType::Factual:     base = 5; break;
    case RequestType::Translation: base = 6; break;
    case RequestType::Unknown:     base = 4; break;
  }
  
  // Adaptive adjustment based on acceptance rate.
  // High acceptance (>70%) → increase depth by 1.
  // Low acceptance (<40%) → decrease depth by 1.
  if (history.totalDrafts < 50)
    return base; // Insufficient data; use base.
  
  if (rate > 0.70f && base < 8)
    return base + 1;
  if (rate < 0.40f && base > 2)
    return base - 1;
  
  return base;
}

void RequestProfile::recordOutcome(RequestType type, uint32_t drafted,
                                   uint32_t accepted) noexcept {
  historyFor(type).record(drafted, accepted);
}

float RequestProfile::acceptanceRate(RequestType type) const noexcept {
  return historyFor(type).acceptanceRate();
}

} // namespace splash::model
