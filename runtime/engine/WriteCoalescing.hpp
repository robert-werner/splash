#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <memory>

namespace splash::engine {

// Write coalescing buffer for SSD tier to reduce write amplification
// Batches multiple small KV page writes into larger sequential writes
struct WriteCoalescingConfig final {
  // Size of coalescing buffer in bytes (default: 4 MB)
  uint64_t bufferSizeBytes = 4 * 1024 * 1024;
  
  // Maximum time to hold writes before flushing (milliseconds)
  double maxCoalesceTimeMs = 50.0;
  
  // Minimum number of pages to coalesce before writing
  uint32_t minPagesToCoalesce = 8;
  
  // Maximum number of pages to coalesce in one batch
  uint32_t maxPagesToCoalesce = 64;
  
  // Enable write combining for sequential page writes
  bool enableSequentialCombining = true;
};

// Statistics from write coalescing
struct WriteCoalescingStats final {
  // Total number of logical page writes requested
  uint64_t logicalWrites = 0;
  
  // Total number of physical writes to disk (after coalescing)
  uint64_t physicalWrites = 0;
  
  // Total bytes written logically
  uint64_t logicalBytesWritten = 0;
  
  // Total bytes written physically (should be less due to coalescing)
  uint64_t physicalBytesWritten = 0;
  
  // Number of times buffer was flushed
  uint64_t flushes = 0;
  
  // Number of pages currently in buffer
  uint32_t bufferedPages = 0;
  
  // Average coalescing ratio (physicalWrites / logicalWrites)
  [[nodiscard]] double coalescingRatio() const noexcept {
    return logicalWrites > 0 ? double(physicalWrites) / double(logicalWrites) : 1.0;
  }
  
  // Write amplification reduction (1.0 = no reduction, 0.5 = 50% reduction)
  [[nodiscard]] double writeAmplificationReduction() const noexcept {
    return 1.0 - coalescingRatio();
  }
};

// A pending write in the coalescing buffer
struct PendingWrite final {
  uint32_t page = 0;
  uint64_t slotOffset = 0;
  double submissionTimeMs = 0.0;
  std::function<void()> completion;
  std::vector<std::byte> data;
};

// Write coalescing buffer manager
class WriteCoalescingBuffer final {
public:
  explicit WriteCoalescingBuffer(const WriteCoalescingConfig &config);
  ~WriteCoalescingBuffer();
  
  // Add a page write to the buffer
  // Returns true if write was buffered, false if buffer is full
  bool addWrite(uint32_t page, uint64_t slotOffset, 
                std::vector<std::byte> data,
                std::function<void()> completion);
  
  // Flush buffered writes to disk if conditions are met
  // Returns number of writes flushed
  uint32_t flushIfNeeded(double nowMs,
                         std::function<void(std::vector<PendingWrite>&)> writer);
  
  // Force flush all buffered writes immediately
  uint32_t forceFlush(std::function<void(std::vector<PendingWrite>&)> writer);
  
  // Get current statistics
  [[nodiscard]] const WriteCoalescingStats& stats() const noexcept { return stats_; }
  
  // Check if buffer should be flushed
  [[nodiscard]] bool shouldFlush(double nowMs) const noexcept;

private:
  WriteCoalescingConfig config_;
  std::vector<PendingWrite> buffer_;
  WriteCoalescingStats stats_;
  double lastFlushTimeMs_ = 0.0;
  uint64_t currentBufferBytes_ = 0;
  
  // Sort and combine sequential writes
  void optimizeWrites(std::vector<PendingWrite> &writes);
};

} // namespace splash::engine
