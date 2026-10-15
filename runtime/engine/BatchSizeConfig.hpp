#pragma once

#include <cstdint>
#include <algorithm>

namespace splash::engine {

// Configurable batch size policy for decode operations
struct BatchSizeConfig final {
  // Minimum batch size (must be at least 1)
  uint32_t minBatchSize = 1;
  
  // Maximum batch size (configurable up to hardware limit)
  uint32_t maxBatchSize = 4;
  
  // Target batch size for optimal throughput
  uint32_t targetBatchSize = 4;
  
  // Enable dynamic batch size adjustment based on load
  bool dynamicAdjustment = true;
  
  // GPU utilization threshold for increasing batch size (percentage)
  uint32_t increaseUtilizationThreshold = 70;
  
  // GPU utilization threshold for decreasing batch size (percentage)
  uint32_t decreaseUtilizationThreshold = 95;
  
  // Minimum number of requests before increasing batch size
  uint32_t minRequestsForIncrease = 2;
  
  [[nodiscard]] bool valid() const noexcept {
    return minBatchSize >= 1 && minBatchSize <= maxBatchSize &&
           targetBatchSize >= minBatchSize && targetBatchSize <= maxBatchSize &&
           increaseUtilizationThreshold < decreaseUtilizationThreshold;
  }
  
  [[nodiscard]] uint32_t clampBatchSize(uint32_t size) const noexcept {
    if (size < minBatchSize) return minBatchSize;
    if (size > maxBatchSize) return maxBatchSize;
    return size;
  }
};

// Statistics for batch size performance
struct BatchSizeStats final {
  // Current batch size being used
  uint32_t currentBatchSize = 1;
  
  // Average GPU utilization (percentage)
  uint32_t avgGpuUtilization = 0;
  
  // Total batches executed
  uint64_t totalBatches = 0;
  
  // Batches by size (indexed by batch size - 1)
  uint64_t batchesBySize[16] = {};
  
  // Total tokens processed
  uint64_t tokensProcessed = 0;
  
  // Average tokens per second
  double avgTokensPerSecond = 0.0;
  
  // Number of times batch size was increased
  uint64_t batchSizeIncreases = 0;
  
  // Number of times batch size was decreased
  uint64_t batchSizeDecreases = 0;
};

// Dynamic batch size controller
class BatchSizeController final {
public:
  explicit BatchSizeController(const BatchSizeConfig &config);
  
  // Adjust batch size based on current conditions
  // Returns new batch size to use
  [[nodiscard]] uint32_t adjustBatchSize(uint32_t currentBatchSize,
                                          uint32_t availableRequests,
                                          uint32_t gpuUtilization);
  
  // Record batch execution
  void recordBatch(uint32_t batchSize, uint32_t tokens, double durationMs);
  
  // Get current statistics
  [[nodiscard]] const BatchSizeStats& stats() const noexcept { return stats_; }
  
  // Update configuration
  void updateConfig(const BatchSizeConfig &config);
  
  // Get current configuration
  [[nodiscard]] const BatchSizeConfig& config() const noexcept { return config_; }

private:
  BatchSizeConfig config_;
  BatchSizeStats stats_;
  
  // Exponential moving average for smoothing
  double utilizationEma_ = 0.0;
  static constexpr double kEmaAlpha = 0.3;
};

} // namespace splash::engine
