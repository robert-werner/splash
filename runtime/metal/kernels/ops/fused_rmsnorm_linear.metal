#include <metal_stdlib>
using namespace metal;

kernel void fusedRMSNormLinear(
    device const float *input [[buffer(0)]],
    device const float *weight [[buffer(1)]],
    device const float *normWeight [[buffer(2)]],
    device float *output [[buffer(3)]],
    constant uint &hiddenSize [[buffer(4)]],
    constant uint &outSize [[buffer(5)]],
    constant float &eps [[buffer(6)]],
    uint2 id [[thread_position_in_grid]]
) {
    uint row = id.y;
    uint col = id.x;
    
    if (col >= outSize) return;
    
    // RMSNorm
    float sumSq = 0.0f;
    for (uint i = 0; i < hiddenSize; ++i) {
        float val = input[row * hiddenSize + i];
        sumSq += val * val;
    }
    float rms = sqrt(sumSq / hiddenSize + eps);
    
    // Fused linear
    float acc = 0.0f;
    for (uint i = 0; i < hiddenSize; ++i) {
        float normed = input[row * hiddenSize + i] / rms * normWeight[i];
        acc += normed * weight[col * hiddenSize + i];
    }
    
    output[row * outSize + col] = acc;
}
