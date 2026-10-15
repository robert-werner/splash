#include "engine/KvDefragmentation.hpp"
#include <algorithm>

namespace splash::engine {

KvDefragmentation::KvDefragmentation(size_t totalPages) 
    : totalPages_(totalPages), pages_(totalPages) {
  for (size_t i = 0; i < totalPages; ++i) {
    pages_[i] = {i, false, 0};
  }
  largestFreeBlock_ = totalPages;
}

float KvDefragmentation::getFragmentation() const {
  if (totalPages_ == 0 || usedPages_ == 0) return 0.0f;
  size_t freePages = totalPages_ - usedPages_;
  if (freePages == 0) return 0.0f;
  return 1.0f - (static_cast<float>(largestFreeBlock_) / freePages);
}

bool KvDefragmentation::needsCompaction(float threshold) const {
  return getFragmentation() > threshold;
}

void KvDefragmentation::markUsed(size_t pageId, size_t blockSize) {
  if (pageId >= totalPages_) return;
  if (!pages_[pageId].inUse) {
    pages_[pageId].inUse = true;
    pages_[pageId].blockSize = blockSize;
    usedPages_++;
    updateStats();
  }
}

void KvDefragmentation::markFree(size_t pageId) {
  if (pageId >= totalPages_) return;
  if (pages_[pageId].inUse) {
    pages_[pageId].inUse = false;
    pages_[pageId].blockSize = 0;
    usedPages_--;
    updateStats();
  }
}

void KvDefragmentation::updateStats() {
  size_t currentBlock = 0;
  largestFreeBlock_ = 0;
  
  for (const auto& page : pages_) {
    if (!page.inUse) {
      currentBlock++;
      largestFreeBlock_ = std::max(largestFreeBlock_, currentBlock);
    } else {
      currentBlock = 0;
    }
  }
}

std::vector<std::pair<size_t, size_t>> KvDefragmentation::getCompactionPlan() {
  std::vector<std::pair<size_t, size_t>> moves;
  
  size_t writePos = 0;
  for (size_t readPos = 0; readPos < totalPages_; ++readPos) {
    if (pages_[readPos].inUse && readPos != writePos) {
      moves.push_back({readPos, writePos});
      writePos++;
    } else if (pages_[readPos].inUse) {
      writePos++;
    }
  }
  
  return moves;
}

} // namespace splash::engine
