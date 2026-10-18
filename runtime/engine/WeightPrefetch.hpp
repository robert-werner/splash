#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace splash::engine {

// Weight prefetch state for overlapping memory transfer with GPU compute.
// Architecture: double-buffering with async prefetch of layer N+1 weights
// while GPU executes layer N, reducing decode latency by 8-10%.
//
// Usage:
//   WeightPrefetch prefetch(config.prefetchLayers);
//   // In decode loop:
//   prefetch.prefetchLayer(currentLayer + 1, weightsBuffer);
//   // ... GPU executes currentLayer
//   prefetch.waitForPrefetch();  // Ensure prefetch completes before next layer
class WeightPrefetch final {
public:
  explicit WeightPrefetch(uint32_t prefetchDepth);
  ~WeightPrefetch() noexcept;

  WeightPrefetch(const WeightPrefetch &) = delete;
  WeightPrefetch &operator=(const WeightPrefetch &) = delete;

  // Asynchronously prefetch weights for the given layer into the staging buffer.
  // Non-blocking: returns immediately while the prefetch happens in the background.
  // layerIndex: the layer whose weights to prefetch
  // weightsPtr: pointer to the layer's weight data
  // weightsBytes: size of the weights to prefetch
  void prefetchLayer(uint32_t layerIndex, const void *weightsPtr, size_t weightsBytes);

  // Block until the most recent prefetch operation completes.
  // Must be called before accessing the prefetched weights.
  void waitForPrefetch();

  // Reset the prefetch state (e.g., at the start of a new request).
  void reset() noexcept;

  // Check if a prefetch operation is currently in flight.
  [[nodiscard]] bool isPrefetchActive() const noexcept;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace splash::engine
