#include "engine/BatchSizeConfig.hpp"
#include <algorithm>

namespace splash::engine {

BatchSizeController::BatchSizeController(const BatchSizeConfig &config)
    : config_(config) {
  stats_.currentBatchSize = config_.targetBatchSize;
}

uint32_t BatchSizeController::adjustBatchSize(uint32_t currentBatchSize,
                                               uint32_t availableRequests,
                                               uint32_t gpuUtilization) {
  // Dynamic adjustment disabled - use target
  if (!config_.dynamicAdjustment) {
    return config_.targetBatchSize;
  }
  
  // Can't increase beyond available requests
  const uint32_t maxPossible = std::min(config_.maxBatchSize, availableRequests);
  
  // Update utilization EMA
  if (utilizationEma_ == 0.0) {
    utilizationEma_ = static_cast<double>(gpuUtilization);
  } else {
    utilizationEma_ = kEmaAlpha * gpuUtilization + (1.0 - kEmaAlpha) * utilizationEma_;
  }
  
  stats_.avgGpuUtilization = static_cast<uint32_t>(utilizationEma_);
  
  uint32_t newBatchSize = currentBatchSize;
  
  // Decrease batch size if GPU is overloaded
  if (utilizationEma_ >= config_.decreaseUtilizationThreshold) {
    if (currentBatchSize > config_.minBatchSize) {
      newBatchSize = currentBatchSize - 1;
      stats_.batchSizeDecreases++;
    }
  }
  // Increase batch size if GPU is underutilized and we have requests
  else if (utilizationEma_ <= config_.increaseUtilizationThreshold) {
    if (currentBatchSize < maxPossible && 
        availableRequests >= config_.minRequestsForIncrease) {
      newBatchSize = currentBatchSize + 1;
      stats_.batchSizeIncreases++;
    }
  }
  
  // Clamp to valid range
  newBatchSize = config_.clampBatchSize(newBatchSize);
  stats_.currentBatchSize = newBatchSize;
  
  return newBatchSize;
}

void BatchSizeController::recordBatch(uint32_t batchSize, uint32_t tokens, 
                                      double durationMs) {
  stats_.totalBatches++;
  stats_.tokensProcessed += tokens;
  
  // Record batch size distribution
  if (batchSize > 0 && batchSize <= 16) {
    stats_.batchesBySize[batchSize - 1]++;
  }
  
  // Update average tokens per second
  if (durationMs > 0.0) {
    const double tokensPerSec = (tokens * 1000.0) / durationMs;
    if (stats_.avgTokensPerSecond == 0.0) {
      stats_.avgTokensPerSecond = tokensPerSec;
    } else {
      // EMA for tokens/sec
      stats_.avgTokensPerSecond = 0.1 * tokensPerSec + 0.9 * stats_.avgTokensPerSecond;
    }
  }
}

void BatchSizeController::updateConfig(const BatchSizeConfig &config) {
  if (config.valid()) {
    config_ = config;
    stats_.currentBatchSize = config_.clampBatchSize(stats_.currentBatchSize);
  }
}

} // namespace splash::engine
