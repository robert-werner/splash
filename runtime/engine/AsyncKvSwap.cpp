#include "engine/AsyncKvSwap.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ios>

namespace splash::engine {

AsyncKvSwap::AsyncKvSwap(Config config)
    : config_(std::move(config)), running_(false) {
  if (config_.enabled && !config_.swapDir.empty()) {
    std::filesystem::create_directories(config_.swapDir);
  }
}

AsyncKvSwap::~AsyncKvSwap() noexcept { stop(); }

void AsyncKvSwap::start() {
  if (!config_.enabled)
    return;

  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return; // Already running

  workerThread_ = std::thread([this]() { workerLoop(); });
}

void AsyncKvSwap::stop() noexcept {
  if (!running_.exchange(false))
    return; // Not running

  {
    std::lock_guard<std::mutex> lock(workerMutex_);
    workerCondition_.notify_all();
  }

  if (workerThread_.joinable()) {
    workerThread_.join();
  }
}

void AsyncKvSwap::touchPage(uint32_t pageId, size_t pageBytes) noexcept {
  if (!config_.enabled)
    return;

  std::lock_guard<std::mutex> lock(pagesMutex_);
  auto &meta = pages_[pageId];
  meta.lastAccessTime = nowMs();
  meta.accessCount++;
  meta.sizeBytes = pageBytes;
  meta.swapped = false;

  // Update RAM usage tracking
  metrics_.currentRamUsageBytes.store(
      metrics_.currentRamUsageBytes.load() + pageBytes);
}

void AsyncKvSwap::registerColdPage(uint32_t pageId) noexcept {
  if (!config_.enabled)
    return;

  std::lock_guard<std::mutex> lock(pagesMutex_);
  coldPageCandidates_.push_back(pageId);
}

void AsyncKvSwap::swapOutColdPages() noexcept {
  if (!config_.enabled)
    return;

  // Check if RAM pressure is high enough to trigger swap
  if (ramUsageFraction() < config_.swapThreshold)
    return;

  swapRequested_.store(true);
  {
    std::lock_guard<std::mutex> lock(workerMutex_);
    workerCondition_.notify_one();
  }
}

bool AsyncKvSwap::isSwapped(uint32_t pageId) const noexcept {
  std::lock_guard<std::mutex> lock(pagesMutex_);
  auto it = pages_.find(pageId);
  return it != pages_.end() && it->second.swapped;
}

void AsyncKvSwap::prefetchPage(uint32_t pageId,
                               std::function<void(bool)> completion) {
  if (!config_.enabled || !completion) {
    if (completion)
      completion(false);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(pagesMutex_);
    auto it = pages_.find(pageId);
    if (it != pages_.end()) {
      it->second.prefetching = true;
    }
  }

  {
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    prefetchQueue_.push_back({pageId, std::move(completion)});
  }

  {
    std::lock_guard<std::mutex> lock(workerMutex_);
    workerCondition_.notify_one();
  }
}

bool AsyncKvSwap::loadPageSync(uint32_t pageId,
                               std::span<std::byte> destination) noexcept {
  if (!config_.enabled)
    return false;

  metrics_.prefetchMissCount.fetch_add(1);
  return swapInPage(pageId, destination);
}

void AsyncKvSwap::workerLoop() noexcept {
  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lock(workerMutex_);
      workerCondition_.wait(lock, [this]() {
        return !running_.load() || swapRequested_.load() ||
               (!prefetchQueue_.empty());
      });

      if (!running_.load())
        break;
    }

    // Process swap-out requests
    if (swapRequested_.exchange(false)) {
      executeSwapOut();
    }

    // Process prefetch requests
    executePrefetch();
  }
}

void AsyncKvSwap::executeSwapOut() noexcept {
  auto coldPages = identifyColdPages();

  for (uint32_t pageId : coldPages) {
    // Get page data (in production, this would come from KvPool)
    std::vector<std::byte> pageData;
    if (pageDataProvider_) {
      pageData = pageDataProvider_(pageId);
    }

    if (pageData.empty())
      continue;

    auto startTime = nowMs();
    bool success = swapOutPage(pageId, pageData);
    auto latency = nowMs() - startTime;

    if (success) {
      metrics_.swapOutCount.fetch_add(1);
      metrics_.totalSwapOutLatencyMs.fetch_add(latency);
      metrics_.bytesSwappedOut.fetch_add(pageData.size());

      // Update page state
      std::lock_guard<std::mutex> lock(pagesMutex_);
      auto it = pages_.find(pageId);
      if (it != pages_.end()) {
        it->second.swapped = true;
        metrics_.currentRamUsageBytes.fetch_sub(it->second.sizeBytes);
      }
    }

    // Check if we've freed enough RAM
    if (ramUsageFraction() < config_.swapThreshold * 0.85)
      break;
  }
}

void AsyncKvSwap::executePrefetch() noexcept {
  std::vector<PrefetchRequest> requests;
  {
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    requests.swap(prefetchQueue_);
  }

  for (auto &req : requests) {
    std::vector<std::byte> pageData;
    bool success = false;

    {
      std::lock_guard<std::mutex> lock(pagesMutex_);
      auto it = swappedPages_.find(req.pageId);
      if (it != swappedPages_.end()) {
        pageData.resize(it->second.originalSizeBytes);
      }
    }

    if (!pageData.empty()) {
      auto startTime = nowMs();
      success = swapInPage(req.pageId, pageData);
      auto latency = nowMs() - startTime;

      if (success) {
        metrics_.swapInCount.fetch_add(1);
        metrics_.prefetchHitCount.fetch_add(1);
        metrics_.totalSwapInLatencyMs.fetch_add(latency);
        metrics_.bytesSwappedIn.fetch_add(pageData.size());

        // Deliver page data to consumer
        if (pageDataConsumer_) {
          pageDataConsumer_(req.pageId, pageData);
        }

        // Update page state
        std::lock_guard<std::mutex> lock(pagesMutex_);
        auto it = pages_.find(req.pageId);
        if (it != pages_.end()) {
          it->second.swapped = false;
          it->second.prefetching = false;
          metrics_.currentRamUsageBytes.fetch_add(it->second.sizeBytes);
        }
      }
    }

    // Invoke completion callback
    if (req.completion) {
      req.completion(success);
    }
  }
}

std::vector<uint32_t> AsyncKvSwap::identifyColdPages() noexcept {
  std::lock_guard<std::mutex> lock(pagesMutex_);

  // Build LRU-sorted list of candidate pages
  std::vector<std::pair<uint64_t, uint32_t>> candidates;
  for (uint32_t pageId : coldPageCandidates_) {
    auto it = pages_.find(pageId);
    if (it != pages_.end() && !it->second.swapped) {
      candidates.emplace_back(it->second.lastAccessTime, pageId);
    }
  }

  // Sort by LRU (oldest first)
  std::sort(candidates.begin(), candidates.end());

  // Select coldest pages until we meet swap target
  std::vector<uint32_t> result;
  size_t targetBytes =
      static_cast<size_t>(config_.ramCapacityBytes * (1.0 - config_.swapThreshold * 0.85));
  size_t freedBytes = 0;

  for (const auto &[_, pageId] : candidates) {
    auto it = pages_.find(pageId);
    if (it != pages_.end()) {
      result.push_back(pageId);
      freedBytes += it->second.sizeBytes;
      if (freedBytes >= targetBytes)
        break;
    }
  }

  coldPageCandidates_.clear();
  return result;
}

bool AsyncKvSwap::swapOutPage(uint32_t pageId,
                              std::span<const std::byte> pageData) noexcept {
  if (pageData.empty())
    return false;

  try {
    auto filePath = swapFilePath(pageId);

    // Compress page data
    kv::KvCompression::CompressionStats stats;
    auto compressed = kv::KvCompression::compress(pageData, stats);

    // Write compressed data (or original if compression not beneficial)
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return false;

    if (stats.compressed && !compressed.empty()) {
      file.write(reinterpret_cast<const char *>(compressed.data()),
                 static_cast<std::streamsize>(compressed.size()));
      if (!file.good())
        return false;

      // Record swapped page metadata
      std::lock_guard<std::mutex> lock(pagesMutex_);
      auto &swapped = swappedPages_[pageId];
      swapped.filePath = filePath;
      swapped.compressedSizeBytes = compressed.size();
      swapped.originalSizeBytes = pageData.size();
      swapped.swapOutTime = nowMs();
      swapped.compressionStats = stats;

      metrics_.currentSwapUsageBytes.fetch_add(compressed.size());
    } else {
      // Write uncompressed
      file.write(reinterpret_cast<const char *>(pageData.data()),
                 static_cast<std::streamsize>(pageData.size()));
      if (!file.good())
        return false;

      std::lock_guard<std::mutex> lock(pagesMutex_);
      auto &swapped = swappedPages_[pageId];
      swapped.filePath = filePath;
      swapped.compressedSizeBytes = pageData.size();
      swapped.originalSizeBytes = pageData.size();
      swapped.swapOutTime = nowMs();
      swapped.compressionStats = stats;

      metrics_.currentSwapUsageBytes.fetch_add(pageData.size());
    }

    return true;
  } catch (...) {
    return false;
  }
}

bool AsyncKvSwap::swapInPage(uint32_t pageId,
                             std::span<std::byte> destination) noexcept {
  try {
    SwappedPage swappedInfo;
    {
      std::lock_guard<std::mutex> lock(pagesMutex_);
      auto it = swappedPages_.find(pageId);
      if (it == swappedPages_.end())
        return false;
      swappedInfo = it->second;
    }

    // Read compressed data from disk
    std::ifstream file(swappedInfo.filePath, std::ios::binary);
    if (!file.is_open())
      return false;

    std::vector<std::byte> compressed(swappedInfo.compressedSizeBytes);
    file.read(reinterpret_cast<char *>(compressed.data()),
              static_cast<std::streamsize>(compressed.size()));
    if (!file.good())
      return false;

    // Decompress if needed
    bool decompressSuccess = true;
    if (swappedInfo.compressionStats.compressed) {
      decompressSuccess =
          kv::KvCompression::decompress(compressed, destination);
    } else {
      // Copy uncompressed data
      if (destination.size() >= compressed.size()) {
        std::memcpy(destination.data(), compressed.data(), compressed.size());
      } else {
        return false;
      }
    }

    if (decompressSuccess) {
      // Clean up swap file
      std::filesystem::remove(swappedInfo.filePath);

      std::lock_guard<std::mutex> lock(pagesMutex_);
      swappedPages_.erase(pageId);
      metrics_.currentSwapUsageBytes.fetch_sub(
          swappedInfo.compressedSizeBytes);
    }

    return decompressSuccess;
  } catch (...) {
    return false;
  }
}

std::filesystem::path AsyncKvSwap::swapFilePath(uint32_t pageId) const {
  return config_.swapDir / ("page_" + std::to_string(pageId) + ".swap");
}

} // namespace splash::engine
