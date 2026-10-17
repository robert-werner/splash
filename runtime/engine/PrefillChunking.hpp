#pragma once

#include <cstdint>
#include <vector>

namespace splash::engine {

// Prefill chunking splits large prefills into smaller chunks to maintain
// interactivity. Large prefills (>8K tokens) can block decode for hundreds
// of milliseconds; chunking allows decode batches to interleave, keeping
// p99 latency bounded even during long prefill operations.
//
// Strategy:
// - Prefills ≤ chunkSize: single-shot, no chunking overhead
// - Prefills > chunkSize: split into chunks of chunkSize tokens
// - Last chunk may be smaller to avoid tiny remainder
// - Chunks processed sequentially with decode interleaving
//
// Default chunk size: 8192 tokens (~200-300ms on M5 Pro 24GB).
class PrefillChunking final {
public:
  static constexpr uint32_t kDefaultChunkSize = 8192;
  static constexpr uint32_t kMinChunkSize = 512;
  static constexpr uint32_t kMaxChunkSize = 32768;

  explicit PrefillChunking(uint32_t chunkSize = kDefaultChunkSize) noexcept
      : chunkSize_(clampChunkSize(chunkSize)) {}

  // Plan chunks for a prefill of totalTokens. Returns token offsets
  // for each chunk start. Empty if totalTokens ≤ chunkSize (single-shot).
  [[nodiscard]] std::vector<uint32_t>
  planChunks(uint32_t totalTokens) const noexcept;

  // Get configured chunk size
  [[nodiscard]] uint32_t chunkSize() const noexcept { return chunkSize_; }

  // Update chunk size (clamped to valid range)
  void setChunkSize(uint32_t size) noexcept {
    chunkSize_ = clampChunkSize(size);
  }

  // Check if prefill requires chunking
  [[nodiscard]] bool requiresChunking(uint32_t totalTokens) const noexcept {
    return totalTokens > chunkSize_;
  }

  // Calculate number of chunks for a given token count
  [[nodiscard]] uint32_t chunkCount(uint32_t totalTokens) const noexcept;

private:
  [[nodiscard]] static uint32_t clampChunkSize(uint32_t size) noexcept {
    if (size < kMinChunkSize)
      return kMinChunkSize;
    if (size > kMaxChunkSize)
      return kMaxChunkSize;
    return size;
  }

  uint32_t chunkSize_;
};

} // namespace splash::engine
