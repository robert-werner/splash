#include "engine/BatchSizeConfig.hpp"

#include <algorithm>

namespace splash::engine {

BatchSizeController::BatchSizeController(const BatchSizeConfig &config)
    : config_(config), stats_{}, utilizationEma_(0.0) {
  stats_.currentBatchSize = config_.targetBatchSize;
}

uint32_t BatchSizeController::adjustBatchSize(uint32_t currentBatchSize,
                                               uint32_t availableRequests,
                                               uint32_t gpuUtilization) {
  if (!config_.dynamicAdjustment) {
    return std::min(availableRequests, config_.maxBatchSize);
  }

  // Update exponential moving average for GPU utilization
  if (stats_.totalBatches == 0) {
    utilizationEma_ = static_cast<double>(gpuUtilization);
  } else {
    utilizationEma_ = kEmaAlpha * gpuUtilization + (1.0 - kEmaAlpha) * utilizationEma_;
  }

  uint32_t newBatchSize = currentBatchSize;

  // Decrease batch size if GPU utilization is too high (memory pressure)
  if (utilizationEma_ > config_.decreaseUtilizationThreshold && 
      currentBatchSize > config_.minBatchSize) {
    newBatchSize = currentBatchSize - 1;
    stats_.batchSizeDecreases++;
  }
  // Increase batch size if GPU utilization is healthy and we have requests
  else if (utilizationEma_ < config_.increaseUtilizationThreshold &&
           currentBatchSize < config_.maxBatchSize &&
           availableRequests >= currentBatchSize + config_.minRequestsForIncrease) {
    newBatchSize = currentBatchSize + 1;
    stats_.batchSizeIncreases++;
  }

  // Clamp to available requests
  newBatchSize = std::min(newBatchSize, availableRequests);
  
  // Clamp to configured limits
  newBatchSize = config_.clampBatchSize(newBatchSize);

  stats_.currentBatchSize = newBatchSize;
  stats_.avgGpuUtilization = static_cast<uint32_t>(utilizationEma_);

  return newBatchSize;
}

void BatchSizeController::recordBatch(uint32_t batchSize, uint32_t tokens, 
                                       double durationMs) {
  if (batchSize > 0 && batchSize <= 16) {
    stats_.batchesBySize[batchSize - 1]++;
  }
  
  stats_.totalBatches++;
  stats_.tokensProcessed += tokens;

  // Update average tokens per second (exponential moving average)
  double tokensPerSecond = (tokens * 1000.0) / std::max(durationMs, 0.001);
  if (stats_.avgTokensPerSecond == 0.0) {
    stats_.avgTokensPerSecond = tokensPerSecond;
  } else {
    stats_.avgTokensPerSecond = 
        kEmaAlpha * tokensPerSecond + (1.0 - kEmaAlpha) * stats_.avgTokensPerSecond;
  }
}

void BatchSizeController::updateConfig(const BatchSizeConfig &config) {
  if (!config.valid()) {
    return;
  }
  config_ = config;
  stats_.currentBatchSize = config_.clampBatchSize(stats_.currentBatchSize);
}

} // namespace splash::engine
