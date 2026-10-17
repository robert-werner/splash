#include <metal_stdlib>
#include <metal_tensor>
#include <MetalPerformancePrimitives/MetalPerformancePrimitives.h>

using namespace metal;
using namespace mpp;
using namespace mpp::tensor_ops;

// Решающая проверка: matmul2d над тензором 4-битных элементов, упакованных
// по два в байт, прямо из device-памяти. Тип элемента объявляется как
// int4b_format, а данные передаются байтовым указателем.
kernel void probe_int4_matmul(
    device bfloat *queries [[buffer(0)]],
    device uchar *keys_packed [[buffer(1)]],
    device float *out [[buffer(2)]]) {
  constexpr ushort M = 8, N = 32, K = 256;

  auto qt = tensor(queries, dextents<int, 2>{K, M}, array<int, 2>{1, K});
  tensor<device int4b_format, dextents<int, 2>, tensor_inline> kt(
      keys_packed, dextents<int, 2>{K, N}, array<int, 2>{1, K});

  auto q0 = qt.slice<K, M>(0, 0);
  auto k0 = kt.slice<K, N>(0, 0);

  constexpr auto descriptor =
      matmul2d_descriptor(M, N, K, false, true, false,
                          matmul2d_descriptor::mode::multiply);
  matmul2d<descriptor, execution_simdgroups<8>> qk;
  auto dest = qk.get_destination_cooperative_tensor<decltype(q0), decltype(k0), float>();
  qk.run(q0, k0, dest);

  for (ushort i = 0; i < dest.get_capacity(); ++i)
    if (dest.is_valid_element(i))
      out[i] = dest[i];
}
