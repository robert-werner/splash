#include <metal_stdlib>
using namespace metal;

kernel void fusedRoPEAttention(
    device const float* q [[buffer(0)]],
    device const float* k [[buffer(1)]],
    device const float* v [[buffer(2)]],
    device float* output [[buffer(3)]],
    constant uint& seqLen [[buffer(4)]],
    constant uint& headDim [[buffer(5)]],
    constant float& ropeBase [[buffer(6)]],
    uint gid [[thread_position_in_grid]]
) {
    uint pos = gid / headDim;
    uint dim = gid % headDim;
    
    float freq = 1.0f / pow(ropeBase, float(dim) / float(headDim));
    float angle = float(pos) * freq;
    float cosVal = cos(angle);
    float sinVal = sin(angle);
    
    float qVal = q[gid];
    uint pairIdx = (dim % 2 == 0) ? gid + 1 : gid - 1;
    float qPair = q[pairIdx];
    
    float qRot = (dim % 2 == 0) ? (qVal * cosVal - qPair * sinVal) : (qVal * cosVal + qPair * sinVal);
    
    float score = 0.0f;
    for (uint i = 0; i < seqLen; ++i) {
        score += qRot * k[i * headDim + dim];
    }
    score /= sqrt(float(headDim));
    
    float out = 0.0f;
    for (uint i = 0; i < seqLen; ++i) {
        out += score * v[i * headDim + dim];
    }
    output[gid] = out;
}
