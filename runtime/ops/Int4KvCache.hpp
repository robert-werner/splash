#pragma once
#include <cstdint>
#include <cstddef>

namespace splash::ops {

struct Int4KvCacheConfig {
  bool enabled = false;
  float quantScale = 1.0f;
  float quantZero = 0.0f;
};

class Int4KvCache {
public:
  explicit Int4KvCache(const Int4KvCacheConfig& config);
  
  size_t getCompressedSize(size_t originalSize) const { return (originalSize + 1) / 2; }

private:
  Int4KvCacheConfig config_;
};

} // namespace splash::ops
