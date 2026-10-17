#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <algorithm>

namespace splash::model {

// Request type classification for adaptive draft depth tuning.
// Different request types exhibit different acceptance rates in speculative
// decoding, warranting different optimal draft depths.
enum class RequestType : uint8_t {
  Code,        // Programming, structured output (6-8 draft depth optimal)
  Creative,    // Fiction, poetry, free-form (2-3 draft depth optimal)
  Factual,     // QA, summaries, retrieval (4-5 draft depth optimal)
  Translation, // Language translation (5-6 draft depth optimal)
  Unknown      // Default fallback (4 draft depth)
};

// Acceptance rate history for one request type, used to tune draft depth
// dynamically based on recent performance.
struct TypeHistory {
  uint32_t totalDrafts{0};
  uint32_t acceptedTokens{0};
  
  [[nodiscard]] float acceptanceRate() const noexcept {
    return totalDrafts > 0 ? static_cast<float>(acceptedTokens) / totalDrafts : 0.5f;
  }
  
  void record(uint32_t drafted, uint32_t accepted) noexcept {
    totalDrafts += drafted;
    acceptedTokens += accepted;
    // Decay after 1000 drafts to adapt to workload shift
    if (totalDrafts > 1000) {
      totalDrafts = (totalDrafts * 3) / 4;
      acceptedTokens = (acceptedTokens * 3) / 4;
    }
  }
};

// Runtime profile tracking for adaptive draft depth selection.
// Heuristics classify request type from the first N tokens, then historical
// acceptance rates guide the optimal draft depth for that type.
class RequestProfile final {
public:
  RequestProfile() = default;
  
  // Classify request type from prompt tokens (first ~50 tokens heuristic).
  [[nodiscard]] static RequestType classifyRequest(
      std::span<const uint32_t> promptTokens) noexcept;
  
  // Optimal draft depth for a request type based on historical acceptance.
  [[nodiscard]] uint32_t optimalDraftDepth(RequestType type) const noexcept;
  
  // Record draft acceptance outcome to update type history.
  void recordOutcome(RequestType type, uint32_t drafted, 
                     uint32_t accepted) noexcept;
  
  // Get current acceptance rate for a type (diagnostic).
  [[nodiscard]] float acceptanceRate(RequestType type) const noexcept;
  
private:
  TypeHistory history_[5]{}; // One per RequestType enum value
  
  [[nodiscard]] TypeHistory& historyFor(RequestType type) noexcept {
    return history_[static_cast<uint8_t>(type)];
  }
  
  [[nodiscard]] const TypeHistory& historyFor(RequestType type) const noexcept {
    return history_[static_cast<uint8_t>(type)];
  }
};

} // namespace splash::model
