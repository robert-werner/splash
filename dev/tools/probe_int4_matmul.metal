#include "metal/kernels/common/kv_extent.h"
#include <metal_tensor>
#include <MetalPerformancePrimitives/MetalPerformancePrimitives.h>

using namespace metal;
using namespace mpp;
using namespace mpp::tensor_ops;

// Проверяет ABI int4: адресацию страницы, байтовые размеры и то, что
// полученный указатель строит тензор int4b_format для аппаратного матмула.
kernel void probe_int4_abi(device bfloat *queries [[buffer(0)]],
                           device float *out [[buffer(1)]],
                           constant SplashKvLayer &kv [[buffer(2)]],
                           device const SplashKvPage *table [[buffer(3)]]) {
  constexpr uint KVHeads = 4;
  using Bytes = SplashKvPageBytes<KVHeads, int4b_format>;
  using Int8Bytes = SplashKvPageBytes<KVHeads, int8_t>;

  // Данные вдвое меньше int8, масштабы те же.
  static_assert(Bytes::Data == Int8Bytes::Data / 2, "int4 did not halve the data bytes");
  static_assert(Bytes::Scale == Int8Bytes::Scale, "int4 did not keep int8 scales");
  static_assert(Bytes::Quantized, "int4 is a quantized format");

  const SplashKvAddressing<KVHeads, int4b_format> addressing(kv, 0);
  SplashKvPageTensors<int4b_format> page = addressing.page(table[0]);

  constexpr ushort M = 8, N = SplashKvPageTokens, D = SplashKvHeadDimension;
  auto qt = tensor(queries, dextents<int, 2>{D, M}, array<int, 2>{1, D});
  // Ключи: 4-битные элементы над байтовым указателем, строки по D элементов.
  tensor<device int4b_format, dextents<int, 2>, tensor_inline> kt(
      page.keys, dextents<int, 2>{D, N}, array<int, 2>{1, D});

  auto q0 = qt.slice<D, M>(0, 0);
  auto k0 = kt.slice<D, N>(0, 0);

  constexpr auto descriptor =
      matmul2d_descriptor(M, N, D, false, true, false,
                          matmul2d_descriptor::mode::multiply);
  matmul2d<descriptor, execution_simdgroups<8>> qk;
  auto dest = qk.get_destination_cooperative_tensor<decltype(q0), decltype(k0), float>();
  qk.run(q0, k0, dest);

  // Масштабы читаются как у int8.
  const float scale = page.key_scales[0];
  for (ushort i = 0; i < dest.get_capacity(); ++i)
    if (dest.is_valid_element(i))
      out[i] = dest[i] * scale;
}
