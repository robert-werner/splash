#include "engine/WriteCoalescing.hpp"
#include <algorithm>
#include <utility>

namespace splash::engine {

WriteCoalescingBuffer::WriteCoalescingBuffer(const WriteCoalescingConfig &config)
    : config_(config) {
  // Pre-allocate buffer to avoid reallocations
  buffer_.reserve(config.maxPagesToCoalesce);
}

WriteCoalescingBuffer::~WriteCoalescingBuffer() = default;

bool WriteCoalescingBuffer::addWrite(uint32_t page, uint64_t slotOffset,
                                      std::vector<std::byte> data,
                                      std::function<void()> completion) {
  // Check if buffer is full
  const uint64_t dataSize = data.size();
  if (currentBufferBytes_ + dataSize > config_.bufferSizeBytes ||
      buffer_.size() >= config_.maxPagesToCoalesce) {
    return false;
  }
  
  // Add write to buffer
  PendingWrite write;
  write.page = page;
  write.slotOffset = slotOffset;
  write.data = std::move(data);
  write.completion = std::move(completion);
  write.submissionTimeMs = 0.0; // Will be set by caller
  
  buffer_.push_back(std::move(write));
  currentBufferBytes_ += dataSize;
  
  stats_.logicalWrites++;
  stats_.logicalBytesWritten += dataSize;
  stats_.bufferedPages = static_cast<uint32_t>(buffer_.size());
  
  return true;
}

bool WriteCoalescingBuffer::shouldFlush(double nowMs) const noexcept {
  if (buffer_.empty()) {
    return false;
  }
  
  // Flush if minimum pages threshold is met
  if (buffer_.size() >= config_.minPagesToCoalesce) {
    return true;
  }
  
  // Flush if oldest write has been waiting too long
  if (!buffer_.empty() && lastFlushTimeMs_ > 0.0) {
    const double timeSinceLastFlush = nowMs - lastFlushTimeMs_;
    if (timeSinceLastFlush >= config_.maxCoalesceTimeMs) {
      return true;
    }
  }
  
  // Flush if buffer is nearly full
  if (currentBufferBytes_ * 10 >= config_.bufferSizeBytes * 9) { // 90% full
    return true;
  }
  
  return false;
}

void WriteCoalescingBuffer::optimizeWrites(std::vector<PendingWrite> &writes) {
  if (!config_.enableSequentialCombining || writes.size() <= 1) {
    return;
  }
  
  // Sort writes by slot offset for sequential access
  std::stable_sort(writes.begin(), writes.end(),
                   [](const PendingWrite &a, const PendingWrite &b) {
                     return a.slotOffset < b.slotOffset;
                   });
  
  // Combine sequential writes where possible
  std::vector<PendingWrite> combined;
  combined.reserve(writes.size());
  
  for (size_t i = 0; i < writes.size(); ++i) {
    if (combined.empty()) {
      combined.push_back(std::move(writes[i]));
      continue;
    }
    
    PendingWrite &last = combined.back();
    PendingWrite &current = writes[i];
    
    // Check if current write is immediately after last write
    const uint64_t lastEnd = last.slotOffset + last.data.size();
    if (current.slotOffset == lastEnd) {
      // Combine data
      last.data.insert(last.data.end(), current.data.begin(), current.data.end());
      
      // Chain completions
      auto lastCompletion = std::move(last.completion);
      auto currentCompletion = std::move(current.completion);
      last.completion = [lastCompletion = std::move(lastCompletion),
                          currentCompletion = std::move(currentCompletion)]() {
        if (lastCompletion) lastCompletion();
        if (currentCompletion) currentCompletion();
      };
    } else {
      combined.push_back(std::move(current));
    }
  }
  
  writes = std::move(combined);
}

uint32_t WriteCoalescingBuffer::flushIfNeeded(
    double nowMs,
    std::function<void(std::vector<PendingWrite>&)> writer) {
  
  if (!shouldFlush(nowMs)) {
    return 0;
  }
  
  return forceFlush(std::move(writer));
}

uint32_t WriteCoalescingBuffer::forceFlush(
    std::function<void(std::vector<PendingWrite>&)> writer) {
  
  if (buffer_.empty()) {
    return 0;
  }
  
  // Optimize writes before flushing
  optimizeWrites(buffer_);
  
  // Update statistics
  const uint32_t pagesFlushed = static_cast<uint32_t>(buffer_.size());
  stats_.physicalWrites += buffer_.size();
  stats_.flushes++;
  
  uint64_t physicalBytes = 0;
  for (const auto &write : buffer_) {
    physicalBytes += write.data.size();
  }
  stats_.physicalBytesWritten += physicalBytes;
  
  // Execute writes
  if (writer) {
    writer(buffer_);
  }
  
  // Clear buffer
  buffer_.clear();
  currentBufferBytes_ = 0;
  stats_.bufferedPages = 0;
  
  return pagesFlushed;
}

} // namespace splash::engine
