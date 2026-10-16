#pragma once

#include <cstdint>
#include <vector>
#include <utility>

namespace splash::engine {

struct DefragmentationPolicy final {
  uint32_t triggerFragmentationRatio = 40;
  uint32_t targetFragmentationRatio = 20;
  double maxDefragTimeMs = 50.0;
  uint32_t maxExtentsPerPass = 10;
  uint32_t minFreePagesForDefrag = 64;
};

struct DefragmentationResult final {
  uint32_t extentsCompacted = 0;
  uint32_t pagesMoved = 0;
  uint32_t fragmentationBefore = 0;
  uint32_t fragmentationAfter = 0;
  double elapsedMs = 0.0;
  uint64_t bytesFreed = 0;
};

class KvDefragmentation final {
public:
  struct PageInfo {
    size_t pageId;
    bool inUse;
    size_t blockSize;
  };

  explicit KvDefragmentation(size_t totalPages);
  
  [[nodiscard]] float getFragmentation() const;
  [[nodiscard]] bool needsCompaction(uint32_t threshold) const noexcept;
  [[nodiscard]] bool shouldDefragment(float fragmentationPercentage) const noexcept {
    return fragmentationPercentage >= 40.0f;
  }
  
  void markUsed(size_t pageId, size_t blockSize);
  void markFree(size_t pageId);
  
  [[nodiscard]] std::vector<std::pair<size_t, size_t>> getCompactionPlan();

private:
  void updateStats();
  
  size_t totalPages_;
  size_t usedPages_ = 0;
  size_t largestFreeBlock_ = 0;
  std::vector<PageInfo> pages_;
};

} // namespace splash::engine
