#include "engine/DynamicBatching.hpp"

#include <algorithm>
#include <cmath>

namespace splash::engine {

void DynamicBatching::observeDecodeBatch(uint32_t batchSize,
                                         double wallMilliseconds,
                                         uint32_t queueDepth,
                                         double gpuUtilization) noexcept {
  if (batchSize == 0 || wallMilliseconds <= 0.0)
    return;

  // Normalize to per-request time for thermal detection
  const double timePerRequest = wallMilliseconds / batchSize;

  // Initialize baseline on first observation
  if (baselineDecodeTime_ == 0.0) {
    baselineDecodeTime_ = timePerRequest;
    if (gpuUtilization > 0.0) {
      gpuUtilizationEma_ = gpuUtilization;
      queueDepthEma_ = static_cast<double>(queueDepth);
    }
    return;
  }

  // Update baseline with exponential moving average
  baselineDecodeTime_ = (1.0 - kBaselineWeight) * baselineDecodeTime_ +
                        kBaselineWeight * timePerRequest;

  // ========== Priority 1: Thermal Throttling Detection ==========
  // Immediate action on 30% slowdown
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
      // Reset GPU feedback counters on thermal action
      consecutiveUnderutilized_ = 0;
      consecutiveOverutilized_ = 0;
      return; // Priority 1 action taken
    }
  }

  // ========== Priority 2: GPU Utilization Feedback ==========
  // Only when GPU utilization is available (gpuUtilization > 0.0)
  if (gpuUtilization > 0.0) {
    // Update EMAs
    gpuUtilizationEma_ = (1.0 - kUtilizationWeight) * gpuUtilizationEma_ +
                         kUtilizationWeight * gpuUtilization;
    queueDepthEma_ = (1.0 - kQueueDepthWeight) * queueDepthEma_ +
                     kQueueDepthWeight * static_cast<double>(queueDepth);

    // Check GPU utilization with hysteresis
    const double lowerBound = targetGpuUtilization_ - kUtilizationMargin;
    const double upperBound = targetGpuUtilization_ + kUtilizationMargin;

    if (gpuUtilizationEma_ < lowerBound) {
      // Underutilized: consider growing batch size
      consecutiveUnderutilized_++;
      consecutiveOverutilized_ = 0;

      if (consecutiveUnderutilized_ >= kUtilizationObservations &&
          currentMaxBatchSize_ < baseMaxBatchSize_ &&
          queueDepthEma_ >= 2.0) { // Ensure sufficient queue depth
        // Increment by 1, conservative growth
        currentMaxBatchSize_ = std::min(baseMaxBatchSize_, currentMaxBatchSize_ + 1);
        consecutiveUnderutilized_ = 0;
        return; // Priority 2 action taken
      }
    } else if (gpuUtilizationEma_ > upperBound) {
      // Overutilized: reduce batch size
      consecutiveOverutilized_++;
      consecutiveUnderutilized_ = 0;

      if (consecutiveOverutilized_ >= kUtilizationObservations &&
          currentMaxBatchSize_ > 1) {
        // Decrement by 1
        currentMaxBatchSize_ = std::max(1u, currentMaxBatchSize_ - 1);
        consecutiveOverutilized_ = 0;
        return; // Priority 2 action taken
      }
    } else {
      // Within target: reset counters
      consecutiveUnderutilized_ = 0;
      consecutiveOverutilized_ = 0;
    }
  }

  // ========== Priority 3: Legacy Recovery ==========
  // Gradual scale back up when performance is good and no GPU feedback action
  if (!isSlow) {
    consecutiveFastBatches_++;
    consecutiveSlowBatches_ = 0;

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
