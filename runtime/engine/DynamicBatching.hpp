#pragma once

#include <cstdint>
#include <chrono>

namespace splash::engine {

// Dynamic batching adjusts maxBatchSize based on GPU utilization, queue depth,
// and thermal state. Three-tier priority: thermal throttling (immediate scale-down),
// GPU utilization feedback (gradual adjustment), legacy recovery (slow growth).
class DynamicBatching final {
public:
  explicit DynamicBatching(uint32_t baseMaxBatchSize, double targetGpuUtilization = 0.98) noexcept
      : baseMaxBatchSize_(baseMaxBatchSize),
        currentMaxBatchSize_(baseMaxBatchSize),
        targetGpuUtilization_(targetGpuUtilization) {}

  // Observe decode batch performance with GPU utilization and queue depth feedback.
  // queueDepth: number of pending requests (decoding + queued from scheduler).
  // gpuUtilization: current GPU utilization [0.0, 1.0], 0.0 for unavailable.
  void observeDecodeBatch(uint32_t batchSize, double wallMilliseconds,
                         uint32_t queueDepth = 0, double gpuUtilization = 0.0) noexcept;
  
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
    gpuUtilizationEma_ = 0.0;
    queueDepthEma_ = 0.0;
    consecutiveUnderutilized_ = 0;
    consecutiveOverutilized_ = 0;
  }

private:
  uint32_t baseMaxBatchSize_;
  uint32_t currentMaxBatchSize_;
  double targetGpuUtilization_;
  
  // Thermal throttling detection (Priority 1)
  double baselineDecodeTime_ = 0.0;
  uint32_t consecutiveSlowBatches_ = 0;
  uint32_t consecutiveFastBatches_ = 0;
  
  // GPU utilization feedback (Priority 2)
  double gpuUtilizationEma_ = 0.0;
  double queueDepthEma_ = 0.0;
  uint32_t consecutiveUnderutilized_ = 0;
  uint32_t consecutiveOverutilized_ = 0;
  
  // Thermal detection thresholds
  static constexpr double kSlowdownThreshold = 1.3;  // 30% slower
  static constexpr uint32_t kSlowBatchesBeforeScaleDown = 3;
  static constexpr uint32_t kFastBatchesBeforeScaleUp = 5;
  static constexpr double kBaselineWeight = 0.1;  // EMA weight
  
  // GPU utilization feedback parameters
  static constexpr double kUtilizationWeight = 0.2;  // EMA weight for GPU util
  static constexpr double kQueueDepthWeight = 0.3;   // EMA weight for queue depth
  static constexpr double kUtilizationMargin = 0.02; // ±2% hysteresis
  static constexpr uint32_t kUtilizationObservations = 4; // Observations before action
};

} // namespace splash::engine
