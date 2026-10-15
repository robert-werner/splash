#include "engine/LazyAneAllocation.hpp"

namespace splash::engine {

LazyAneAllocator::LazyAneAllocator(const LazyAnePolicy &policy)
    : policy_(policy) {}

LazyAneAllocator::~LazyAneAllocator() = default;

bool LazyAneAllocator::shouldAllocate(uint32_t tokensProcessed, double /* nowMs */) const noexcept {
  // Already allocated
  if (stats_.allocated) {
    return false;
  }
  
  // Lazy allocation disabled - allocate immediately
  if (!policy_.enabled) {
    return true;
  }
  
  // Check if minimum tokens threshold is met
  if (tokensProcessed < policy_.minTokensBeforeAllocation) {
    return false;
  }
  
  // Ready to allocate
  return true;
}

bool LazyAneAllocator::shouldRelease(double nowMs) const noexcept {
  // Not allocated
  if (!stats_.allocated) {
    return false;
  }
  
  // Usage tracking disabled
  if (!policy_.trackUsage) {
    return false;
  }
  
  // Check idle timeout
  const double idleTime = nowMs - stats_.lastUsageTimeMs;
  return idleTime >= policy_.idleReleaseMs;
}

void LazyAneAllocator::markAllocated(double nowMs, uint64_t allocatedBytes) {
  if (!stats_.allocated) {
    stats_.allocated = true;
    stats_.allocationTimeMs = nowMs;
    stats_.lastUsageTimeMs = nowMs;
    stats_.allocations++;
    
    // Calculate memory saved by delaying allocation
    if (stats_.allocations > 1 && stats_.allocationTimeMs > 0.0) {
      // Estimate memory saved based on delay
      stats_.memorySavedBytes += allocatedBytes;
    }
  }
}

void LazyAneAllocator::markReleased(double /* nowMs */) {
  if (stats_.allocated) {
    stats_.allocated = false;
    stats_.releases++;
  }
}

void LazyAneAllocator::recordUsage(uint32_t tokens, double nowMs) {
  stats_.tokensProcessed += tokens;
  stats_.lastUsageTimeMs = nowMs;
}

} // namespace splash::engine
