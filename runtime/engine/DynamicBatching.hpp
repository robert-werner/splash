#pragma once

#include <array>
#include <cstdint>
#include <chrono>

namespace splash::engine {

// Dynamic batching adjusts maxBatchSize based on GPU utilization, queue depth,
// and thermal state. Three-tier priority: thermal throttling (immediate scale-down),
// GPU utilization feedback (gradual adjustment), legacy recovery (slow growth).
// Tracks P99 latency to constrain growth when latency exceeds target.
class DynamicBatching final {
public:
  explicit DynamicBatching(uint32_t baseMaxBatchSize, double targetGpuUtilization = 0.98,
                          double targetP99LatencyMs = 150.0) noexcept
      : baseMaxBatchSize_(baseMaxBatchSize),
        currentMaxBatchSize_(baseMaxBatchSize),
        targetGpuUtilization_(targetGpuUtilization),
        targetP99LatencyMs_(targetP99LatencyMs) {}

  // Observe decode batch performance with GPU utilization and queue depth feedback.
  // queueDepth: number of pending requests (decoding + queued from scheduler).
  // gpuUtilization: current GPU utilization [0.0, 1.0], 0.0 for unavailable.
  void observeDecodeBatch(uint32_t batchSize, double wallMilliseconds,
                         uint32_t queueDepth = 0, double gpuUtilization = 0.0) noexcept;
  
  // Get current recommended max batch size
  [[nodiscard]] uint32_t currentMaxBatchSize() const noexcept {
    return currentMaxBatchSize_;
  }
  
  // Get current P99 latency estimate
  [[nodiscard]] double currentP99Latency() const noexcept {
    return estimateP99();
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
    latencyHistogramCount_ = 0;
    latencyHistogram_.fill(0);
  }

private:
  // Estimate P99 latency from histogram
  [[nodiscard]] double estimateP99() const noexcept;
  
  uint32_t baseMaxBatchSize_;
  uint32_t currentMaxBatchSize_;
  double targetGpuUtilization_;
  double targetP99LatencyMs_;
  
  // Thermal throttling detection (Priority 1)
  double baselineDecodeTime_ = 0.0;
  uint32_t consecutiveSlowBatches_ = 0;
  uint32_t consecutiveFastBatches_ = 0;
  
  // GPU utilization feedback (Priority 2)
  double gpuUtilizationEma_ = 0.0;
  double queueDepthEma_ = 0.0;
  uint32_t consecutiveUnderutilized_ = 0;
  uint32_t consecutiveOverutilized_ = 0;
  
  // P99 latency tracking via histogram
  static constexpr size_t kLatencyHistogramBuckets = 100;
  std::array<uint32_t, kLatencyHistogramBuckets> latencyHistogram_{};
  uint32_t latencyHistogramCount_ = 0;
  
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
  
  // P99 latency histogram parameters
  static constexpr double kLatencyBucketSizeMs = 5.0; // 5ms per bucket
  static constexpr uint32_t kLatencyHistogramWindow = 1000; // rolling window
};

} // namespace splash::engine
