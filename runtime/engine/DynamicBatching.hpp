#pragma once

#include <cstdint>
#include <chrono>

namespace splash::engine {

// Dynamic batching adjusts maxBatchSize based on system load and thermal state.
// When enabled, monitors decode performance and scales batch size down during
// thermal throttling, then gradually recovers when conditions improve.
class DynamicBatching final {
public:
  explicit DynamicBatching(uint32_t baseMaxBatchSize) noexcept
      : baseMaxBatchSize_(baseMaxBatchSize),
        currentMaxBatchSize_(baseMaxBatchSize) {}

  // Observe decode batch performance to detect thermal throttling
  void observeDecodeBatch(uint32_t batchSize, double wallMilliseconds) noexcept;
  
  // Get current recommended max batch size
  [[nodiscard]] uint32_t currentMaxBatchSize() const noexcept {
    return currentMaxBatchSize_;
  }
  
  // Reset to base configuration
  void reset() noexcept {
    currentMaxBatchSize_ = baseMaxBatchSize_;
    consecutiveSlowBatches_ = 0;
    consecutiveFastBatches_ = 0;
    baselineDecodeTime_ = 0.0;
  }

private:
  uint32_t baseMaxBatchSize_;
  uint32_t currentMaxBatchSize_;
  
  // Performance tracking
  double baselineDecodeTime_ = 0.0;
  uint32_t consecutiveSlowBatches_ = 0;
  uint32_t consecutiveFastBatches_ = 0;
  
  // Thresholds for thermal detection
  static constexpr double kSlowdownThreshold = 1.3;  // 30% slower
  static constexpr uint32_t kSlowBatchesBeforeScaleDown = 3;
  static constexpr uint32_t kFastBatchesBeforeScaleUp = 5;
  static constexpr double kBaselineWeight = 0.1;  // EMA weight
};

} // namespace splash::engine
