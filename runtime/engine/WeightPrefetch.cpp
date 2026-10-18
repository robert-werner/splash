#include "engine/WeightPrefetch.hpp"

#include <algorithm>
#include <cstring>
#include <dispatch/dispatch.h>

namespace splash::engine {

struct WeightPrefetch::Impl {
  uint32_t prefetchDepth;
  dispatch_queue_t prefetchQueue;
  dispatch_semaphore_t prefetchSemaphore;
  bool prefetchActive = false;
  std::optional<uint32_t> lastPrefetchedLayer;

  explicit Impl(uint32_t depth)
      : prefetchDepth(depth),
        prefetchQueue(dispatch_queue_create("com.splash.weight_prefetch",
                                             DISPATCH_QUEUE_SERIAL)),
        prefetchSemaphore(dispatch_semaphore_create(0)) {}

  ~Impl() {
    if (prefetchQueue) {
      dispatch_release(prefetchQueue);
    }
    if (prefetchSemaphore) {
      dispatch_release(prefetchSemaphore);
    }
  }
};

WeightPrefetch::WeightPrefetch(uint32_t prefetchDepth)
    : impl_(new Impl(prefetchDepth)) {}

WeightPrefetch::~WeightPrefetch() noexcept {
  if (impl_) {
    // Wait for any in-flight prefetch to complete
    if (impl_->prefetchActive) {
      dispatch_semaphore_wait(impl_->prefetchSemaphore, DISPATCH_TIME_FOREVER);
    }
    delete impl_;
  }
}

void WeightPrefetch::prefetchLayer(uint32_t layerIndex, const void *weightsPtr,
                                   size_t weightsBytes) {
  if (!impl_ || !weightsPtr || weightsBytes == 0) {
    return;
  }

  // Wait for previous prefetch to complete before starting a new one
  if (impl_->prefetchActive) {
    waitForPrefetch();
  }

  // Skip if already prefetched
  if (impl_->lastPrefetchedLayer && *impl_->lastPrefetchedLayer == layerIndex) {
    return;
  }

  impl_->prefetchActive = true;
  impl_->lastPrefetchedLayer = layerIndex;

  // Dispatch async prefetch operation
  dispatch_async(impl_->prefetchQueue, ^{
    // Touch memory pages to bring them into cache
    // Use volatile to prevent compiler optimization
    volatile uint8_t dummy = 0;
    const uint8_t *ptr = static_cast<const uint8_t *>(weightsPtr);
    const size_t pageSize = 4096; // macOS page size
    
    for (size_t offset = 0; offset < weightsBytes; offset += pageSize) {
      dummy += ptr[offset];
    }
    // Touch the last byte
    dummy += ptr[weightsBytes - 1];
    (void)dummy; // Suppress unused warning

    // Signal completion
    dispatch_semaphore_signal(impl_->prefetchSemaphore);
  });
}

void WeightPrefetch::waitForPrefetch() {
  if (!impl_ || !impl_->prefetchActive) {
    return;
  }

  // Wait for the prefetch to complete
  dispatch_semaphore_wait(impl_->prefetchSemaphore, DISPATCH_TIME_FOREVER);
  impl_->prefetchActive = false;
}

void WeightPrefetch::reset() noexcept {
  if (!impl_) {
    return;
  }

  // Wait for any in-flight prefetch
  if (impl_->prefetchActive) {
    dispatch_semaphore_wait(impl_->prefetchSemaphore, DISPATCH_TIME_FOREVER);
    impl_->prefetchActive = false;
  }

  impl_->lastPrefetchedLayer.reset();
}

bool WeightPrefetch::isPrefetchActive() const noexcept {
  return impl_ && impl_->prefetchActive;
}

} // namespace splash::engine
