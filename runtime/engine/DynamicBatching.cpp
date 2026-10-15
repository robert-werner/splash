#include "engine/DynamicBatching.hpp"

#include <algorithm>
#include <cmath>

namespace splash::engine {

void DynamicBatching::observeDecodeBatch(uint32_t batchSize,
                                         double wallMilliseconds) noexcept {
  if (batchSize == 0 || wallMilliseconds <= 0.0)
    return;

  // Normalize to per-request time for comparison across batch sizes
  const double timePerRequest = wallMilliseconds / batchSize;

  // Initialize baseline on first observation
  if (baselineDecodeTime_ == 0.0) {
    baselineDecodeTime_ = timePerRequest;
    return;
  }

  // Update baseline with exponential moving average
  baselineDecodeTime_ = (1.0 - kBaselineWeight) * baselineDecodeTime_ +
                        kBaselineWeight * timePerRequest;

  // Detect thermal throttling: performance significantly worse than baseline
  const bool isSlow = timePerRequest > baselineDecodeTime_ * kSlowdownThreshold;

  if (isSlow) {
    consecutiveSlowBatches_++;
    consecutiveFastBatches_ = 0;

    // Scale down batch size if consistently slow
    if (consecutiveSlowBatches_ >= kSlowBatchesBeforeScaleDown &&
        currentMaxBatchSize_ > 1) {
      // Reduce by ~30% but stay at least 1
      currentMaxBatchSize_ =
          std::max(1u, static_cast<uint32_t>(currentMaxBatchSize_ * 0.7));
      consecutiveSlowBatches_ = 0;
    }
  } else {
    consecutiveFastBatches_++;
    consecutiveSlowBatches_ = 0;

    // Gradually scale back up when performance is good
    if (consecutiveFastBatches_ >= kFastBatchesBeforeScaleUp &&
        currentMaxBatchSize_ < baseMaxBatchSize_) {
      // Increase by 1, conservative recovery
      currentMaxBatchSize_ =
          std::min(baseMaxBatchSize_, currentMaxBatchSize_ + 1);
      consecutiveFastBatches_ = 0;
    }
  }
}

} // namespace splash::engine
