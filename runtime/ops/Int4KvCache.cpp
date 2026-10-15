#include "ops/Int4KvCache.hpp"
#include <algorithm>
#include <cmath>

namespace splash::ops {

Int4KvCache::Int4KvCache(const Int4KvCacheConfig& config) : config_(config) {}

void Int4KvCache::quantize(const float* input, uint8_t* output, size_t count) {
  if (!config_.enabled) return;
  
  for (size_t i = 0; i + 1 < count; i += 2) {
    float v0 = input[i] / config_.quantScale;
    float v1 = input[i + 1] / config_.quantScale;
    int8_t q0 = static_cast<int8_t>(std::clamp(std::round(v0), -8.0f, 7.0f));
    int8_t q1 = static_cast<int8_t>(std::clamp(std::round(v1), -8.0f, 7.0f));
    output[i / 2] = packInt4(q0, q1);
  }
  
  if (count % 2 == 1) {
    float v = input[count - 1] / config_.quantScale;
    int8_t q = static_cast<int8_t>(std::clamp(std::round(v), -8.0f, 7.0f));
    output[count / 2] = packInt4(q, 0);
  }
}

void Int4KvCache::dequantize(const uint8_t* input, float* output, size_t count) {
  if (!config_.enabled) return;
  
  for (size_t i = 0; i + 1 < count; i += 2) {
    int8_t q0, q1;
    unpackInt4(input[i / 2], q0, q1);
    output[i] = static_cast<float>(q0) * config_.quantScale;
    output[i + 1] = static_cast<float>(q1) * config_.quantScale;
  }
  
  if (count % 2 == 1) {
    int8_t q0, q1;
    unpackInt4(input[count / 2], q0, q1);
    output[count - 1] = static_cast<float>(q0) * config_.quantScale;
  }
}

} // namespace splash::ops
