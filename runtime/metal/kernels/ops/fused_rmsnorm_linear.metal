#include <metal_stdlib>
using namespace metal;

kernel void fusedRMSNormLinear(
    device const float* input [[buffer(0)]],
    device const float* weight [[buffer(1)]],
    device float* output [[buffer(2)]],
    constant uint& dim [[buffer(3)]],
    constant float& eps [[buffer(4)]],
    uint tid [[thread_position_in_grid]]
) {
    float sumSq = 0.0f;
    for (uint i = 0; i < dim; ++i) {
        float val = input[tid * dim + i];
        sumSq += val * val;
    }
    float rms = sqrt(sumSq / float(dim) + eps);
    for (uint i = 0; i < dim; ++i) {
        float normed = input[tid * dim + i] / rms;
        float acc = 0.0f;
        for (uint j = 0; j < dim; ++j) {
            acc += normed * weight[i * dim + j];
        }
        output[tid * dim + i] = acc;
    }
}
