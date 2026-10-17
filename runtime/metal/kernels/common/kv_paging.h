#pragma once

#include "metal/abi/ExecutionGeometry.h"
#include "metal/abi/KvExtent.h"
#include <metal_stdlib>

using namespace metal;

// KV page geometry of both formats, shared by the attention and store
// kernels. A page holds SplashKvPageTokens tokens of every KV head: keys
// token-major, values dimension-major. INT8 adds one fp32 scale per (KV head,
// token) for each tensor.
constant uint SplashKvPageTokens = SPLASH_TARGET_KV_BLOCK_TOKENS;
constant uint SplashKvHeadDimension = SPLASH_KV_HEAD_DIMENSION;

// An element's index in a layer's region of one tensor: its page's slab,
// then its place in the slab (abi/KvExtent.h).
template <uint KVHeads>
inline ulong splash_kv_key_index(uint page, uint head, uint token,
                                   uint dimension) {
  constexpr ulong ElementsPerPage =
      ulong(KVHeads) * SplashKvPageTokens * SplashKvHeadDimension;
  return ulong(page) * ElementsPerPage +
         splash_kv_key_element(head, token, dimension);
}

template <uint KVHeads>
inline ulong splash_kv_value_index(uint page, uint head, uint token,
                                     uint dimension) {
  constexpr ulong ElementsPerPage =
      ulong(KVHeads) * SplashKvPageTokens * SplashKvHeadDimension;
  return ulong(page) * ElementsPerPage +
         splash_kv_value_element(head, token, dimension);
}

// INT4 stores values token-major, the way keys are stored, so that the two
// elements sharing a byte are adjacent dimensions of one token: one thread
// owns the byte and no two writers meet. Every other format keeps the
// dimension-major value layout. The attention kernel reads the tensor with
// the matching extents, so the PV product transposes one and not the other.
template <uint KVHeads, typename CacheElement>
inline ulong splash_kv_value_index_of(uint page, uint head, uint token,
                                      uint dimension) {
  if constexpr (is_same<CacheElement, int4b_format>::value)
    return splash_kv_key_index<KVHeads>(page, head, token, dimension);
  return splash_kv_value_index<KVHeads>(page, head, token, dimension);
}

template <uint KVHeads>
inline ulong splash_q8_scale_index(uint page, uint head, uint token) {
  constexpr ulong ScalesPerPage = ulong(KVHeads) * SplashKvPageTokens;
  return ulong(page) * ScalesPerPage + splash_kv_scale_element(head, token);
}
