#pragma once

#include <cstdint>
#include <optional>

namespace splash::engine {

// Policy for lazy ANE allocation to reduce memory pressure
struct LazyAnePolicy final {
  // Enable lazy allocation (defer ANE buffer allocation until first use)
  bool enabled = true;
  
  // Reserve memory for ANE but don't allocate until needed
  bool reserveOnly = true;
  
  // Minimum tokens processed before allocating ANE buffers
  uint32_t minTokensBeforeAllocation = 0;
  
  // Release ANE buffers after idle period (milliseconds)
  double idleReleaseMs = 300000.0; // 5 minutes
  
  // Track last usage time for automatic release
  bool trackUsage = true;
};

// Statistics from lazy ANE allocation
struct LazyAneStats final {
  // Whether ANE buffers are currently allocated
  bool allocated = false;
  
  // Time when ANE was allocated (milliseconds)
  double allocationTimeMs = 0.0;
  
  // Time when ANE was last used (milliseconds)
  double lastUsageTimeMs = 0.0;
  
  // Number of times ANE was allocated
  uint64_t allocations = 0;
  
  // Number of times ANE was released
  uint64_t releases = 0;
  
  // Total tokens processed with ANE
  uint64_t tokensProcessed = 0;
  
  // Memory saved by lazy allocation (bytes)
  uint64_t memorySavedBytes = 0;
};

// Lazy ANE allocator interface
class LazyAneAllocator final {
public:
  explicit LazyAneAllocator(const LazyAnePolicy &policy);
  ~LazyAneAllocator();
  
  // Check if ANE should be allocated now
  [[nodiscard]] bool shouldAllocate(uint32_t tokensProcessed, double nowMs) const noexcept;
  
  // Check if ANE should be released due to inactivity
  [[nodiscard]] bool shouldRelease(double nowMs) const noexcept;
  
  // Mark ANE as allocated
  void markAllocated(double nowMs, uint64_t allocatedBytes);
  
  // Mark ANE as released
  void markReleased(double nowMs);
  
  // Record ANE usage
  void recordUsage(uint32_t tokens, double nowMs);
  
  // Get current statistics
  [[nodiscard]] const LazyAneStats& stats() const noexcept { return stats_; }
  
  // Check if ANE is currently allocated
  [[nodiscard]] bool isAllocated() const noexcept { return stats_.allocated; }

private:
  LazyAnePolicy policy_;
  LazyAneStats stats_;
};

} // namespace splash::engine
