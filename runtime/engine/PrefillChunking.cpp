#include "engine/PrefillChunking.hpp"

#include <algorithm>

namespace splash::engine {

std::vector<uint32_t>
PrefillChunking::planChunks(uint32_t totalTokens) const noexcept {
  std::vector<uint32_t> chunks;

  if (totalTokens <= chunkSize_) {
    // Single-shot prefill, no chunking
    return chunks;
  }

  // Calculate number of full chunks and remainder
  const uint32_t fullChunks = totalTokens / chunkSize_;
  const uint32_t remainder = totalTokens % chunkSize_;

  // Plan chunk offsets
  for (uint32_t i = 0; i < fullChunks; ++i) {
    chunks.push_back(i * chunkSize_);
  }

  // Handle remainder: if small (<25% of chunkSize), merge into last chunk;
  // otherwise create a separate chunk
  if (remainder > 0) {
    const uint32_t mergeThreshold = chunkSize_ / 4;
    if (remainder < mergeThreshold && !chunks.empty()) {
      // Merge remainder into last chunk by not adding a new offset
      // (last chunk will naturally extend to totalTokens)
    } else {
      // Create separate chunk for remainder
      chunks.push_back(fullChunks * chunkSize_);
    }
  }

  return chunks;
}

uint32_t PrefillChunking::chunkCount(uint32_t totalTokens) const noexcept {
  if (totalTokens <= chunkSize_)
    return 1;

  const uint32_t fullChunks = totalTokens / chunkSize_;
  const uint32_t remainder = totalTokens % chunkSize_;

  if (remainder == 0)
    return fullChunks;

  const uint32_t mergeThreshold = chunkSize_ / 4;
  if (remainder < mergeThreshold)
    return fullChunks; // Remainder merged into last chunk
  else
    return fullChunks + 1; // Separate chunk for remainder
}

} // namespace splash::engine
