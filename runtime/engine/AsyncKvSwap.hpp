#pragma once

#include "ops/KvCompression.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace splash::engine {

// Async background swap system for KV cache pages to SSD with compression.
// Manages cold page eviction and hot page prefetching to extend effective
// context from 262K (RAM) to 1M+ (compressed SSD swap).
//
// Design:
// - Background thread handles all swap I/O asynchronously
// - LRU policy identifies cold pages when RAM usage > 90% capacity
// - Pages compressed via KvCompression before SSD write (10-15% savings)
// - Predictive prefetch for swapped pages with async decompression
// - Target latency: <30ms per 4 MiB page (hidden by 76ms decode cycle)
//
// Thread-safety: all public methods are thread-safe.
class AsyncKvSwap final {
public:
  struct Config {
    std::filesystem::path swapDir;
    size_t ramCapacityBytes = 0;
    double swapThreshold = 0.90; // Swap when RAM > 90% capacity
    uint32_t backgroundThreads = 1;
    bool enabled = false;
  };

  struct PageMetrics {
    uint64_t lastAccessTime = 0;
    uint64_t accessCount = 0;
    size_t sizeBytes = 0;
    bool swapped = false;
    bool prefetching = false;
  };

  struct SwapMetrics {
    uint64_t swapOutCount = 0;
    uint64_t swapInCount = 0;
    uint64_t prefetchHitCount = 0;
    uint64_t prefetchMissCount = 0;
    uint64_t bytesSwappedOut = 0;
    uint64_t bytesSwappedIn = 0;
    uint64_t totalSwapOutLatencyMs = 0;
    uint64_t totalSwapInLatencyMs = 0;
    size_t currentRamUsageBytes = 0;
    size_t currentSwapUsageBytes = 0;
  };

  struct AtomicSwapMetrics {
    std::atomic<uint64_t> swapOutCount{0};
    std::atomic<uint64_t> swapInCount{0};
    std::atomic<uint64_t> prefetchHitCount{0};
    std::atomic<uint64_t> prefetchMissCount{0};
    std::atomic<uint64_t> bytesSwappedOut{0};
    std::atomic<uint64_t> bytesSwappedIn{0};
    std::atomic<uint64_t> totalSwapOutLatencyMs{0};
    std::atomic<uint64_t> totalSwapInLatencyMs{0};
    std::atomic<size_t> currentRamUsageBytes{0};
    std::atomic<size_t> currentSwapUsageBytes{0};
  };

  explicit AsyncKvSwap(Config config);
  ~AsyncKvSwap() noexcept;

  AsyncKvSwap(const AsyncKvSwap &) = delete;
  AsyncKvSwap &operator=(const AsyncKvSwap &) = delete;

  // Start background swap thread
  void start();

  // Stop background thread and flush pending operations
  void stop() noexcept;

  // Register page access for LRU tracking
  void touchPage(uint32_t pageId, size_t pageBytes) noexcept;

  // Mark page as cold candidate for swap-out
  void registerColdPage(uint32_t pageId) noexcept;

  // Request async swap-out of cold pages if RAM pressure high
  void swapOutColdPages() noexcept;

  // Check if page is currently swapped to disk
  [[nodiscard]] bool isSwapped(uint32_t pageId) const noexcept;

  // Request prefetch of swapped page (async decompression + load)
  void prefetchPage(uint32_t pageId,
                    std::function<void(bool success)> completion);

  // Synchronous load of swapped page (fallback if prefetch missed)
  [[nodiscard]] bool loadPageSync(uint32_t pageId,
                                  std::span<std::byte> destination) noexcept;

  // Get swap metrics (returns snapshot, not atomic copy)
  [[nodiscard]] SwapMetrics snapshotMetrics() const noexcept;

  // Current RAM usage as fraction of capacity
  [[nodiscard]] double ramUsageFraction() const noexcept {
    if (config_.ramCapacityBytes == 0)
      return 0.0;
    return static_cast<double>(metrics_.currentRamUsageBytes.load()) /
           static_cast<double>(config_.ramCapacityBytes);
  }

  // Average swap latencies
  [[nodiscard]] double avgSwapOutLatencyMs() const noexcept {
    uint64_t count = metrics_.swapOutCount.load();
    if (count == 0)
      return 0.0;
    return static_cast<double>(metrics_.totalSwapOutLatencyMs.load()) /
           static_cast<double>(count);
  }

  [[nodiscard]] double avgSwapInLatencyMs() const noexcept {
    uint64_t count = metrics_.swapInCount.load();
    if (count == 0)
      return 0.0;
    return static_cast<double>(metrics_.totalSwapInLatencyMs.load()) /
           static_cast<double>(count);
  }

private:
  struct SwappedPage {
    std::filesystem::path filePath;
    size_t compressedSizeBytes = 0;
    size_t originalSizeBytes = 0;
    uint64_t swapOutTime = 0;
    kv::KvCompression::CompressionStats compressionStats;
  };

  struct PrefetchRequest {
    uint32_t pageId;
    std::function<void(bool)> completion;
  };

  // Background worker loop
  void workerLoop() noexcept;

  // Execute swap-out of cold pages
  void executeSwapOut() noexcept;

  // Execute prefetch requests
  void executePrefetch() noexcept;

  // Identify cold pages via LRU policy
  [[nodiscard]] std::vector<uint32_t> identifyColdPages() noexcept;

  // Write page to swap file with compression
  [[nodiscard]] bool swapOutPage(uint32_t pageId,
                                 std::span<const std::byte> pageData) noexcept;

  // Read page from swap file with decompression
  [[nodiscard]] bool swapInPage(uint32_t pageId,
                                std::span<std::byte> destination) noexcept;

  // Get swap file path for page
  [[nodiscard]] std::filesystem::path swapFilePath(uint32_t pageId) const;

  // Current timestamp in milliseconds
  [[nodiscard]] static uint64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  Config config_;
  AtomicSwapMetrics metrics_;

  // Page tracking (protected by pagesMutex_)
  mutable std::mutex pagesMutex_;
  std::unordered_map<uint32_t, PageMetrics> pages_;
  std::unordered_map<uint32_t, SwappedPage> swappedPages_;
  std::vector<uint32_t> coldPageCandidates_;

  // Prefetch queue (protected by prefetchMutex_)
  mutable std::mutex prefetchMutex_;
  std::vector<PrefetchRequest> prefetchQueue_;

  // Background worker thread
  std::thread workerThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> swapRequested_{false};
  std::mutex workerMutex_;
  std::condition_variable workerCondition_;

  // Page data callback (injected for testing)
  std::function<std::vector<std::byte>(uint32_t pageId)> pageDataProvider_;
  std::function<void(uint32_t pageId, std::span<const std::byte> data)>
      pageDataConsumer_;
};

} // namespace splash::engine
