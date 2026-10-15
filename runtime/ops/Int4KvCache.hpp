#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace splash::ops {

struct Int4KvCacheConfig {
  bool enabled = false;
  float quantScale = 8.0f;
  float quantZero = 0.0f;
};

class Int4KvCache {
public:
  explicit Int4KvCache(const Int4KvCacheConfig& config);
  
  size_t getCompressedSize(size_t originalSize) const { return (originalSize + 1) / 2; }
  
  void quantize(const float* input, uint8_t* output, size_t count);
  void dequantize(const uint8_t* input, float* output, size_t count);

private:
  Int4KvCacheConfig config_;
  
  inline uint8_t packInt4(int8_t low, int8_t high) const {
    return (uint8_t(high & 0x0F) << 4) | uint8_t(low & 0x0F);
  }
  
  inline void unpackInt4(uint8_t packed, int8_t& low, int8_t& high) const {
    low = int8_t(packed & 0x0F);
    high = int8_t((packed >> 4) & 0x0F);
    if (low > 7) low -= 16;
    if (high > 7) high -= 16;
  }
};

} // namespace splash::ops
