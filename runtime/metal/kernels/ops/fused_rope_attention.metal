#include <metal_stdlib>
using namespace metal;

kernel void fusedRoPEAttention(
    device const float *query [[buffer(0)]],
    device const float *key [[buffer(1)]],
    device const float *value [[buffer(2)]],
    device float *output [[buffer(3)]],
    constant uint &seqLen [[buffer(4)]],
    constant uint &headDim [[buffer(5)]],
    constant uint &numHeads [[buffer(6)]],
    constant float &theta [[buffer(7)]],
    uint3 id [[thread_position_in_grid]]
) {
    uint head = id.z;
    uint pos = id.y;
    uint dim = id.x;
    
    if (head >= numHeads || pos >= seqLen || dim >= headDim) return;
    
    uint qIdx = (pos * numHeads + head) * headDim + dim;
    uint kIdx = qIdx;
    
    // RoPE
    float freq = 1.0f / pow(theta, float(dim / 2 * 2) / headDim);
    float angle = float(pos) * freq;
    float cosVal = cos(angle);
    float sinVal = sin(angle);
    
    float q0 = query[qIdx];
    float q1 = query[qIdx + 1];
    float qRot0 = q0 * cosVal - q1 * sinVal;
    float qRot1 = q0 * sinVal + q1 * cosVal;
    
    float k0 = key[kIdx];
    float k1 = key[kIdx + 1];
    float kRot0 = k0 * cosVal - k1 * sinVal;
    float kRot1 = k0 * sinVal + k1 * cosVal;
    
    // Attention score
    float score = 0.0f;
    for (uint i = 0; i < headDim; i += 2) {
        uint idx = (pos * numHeads + head) * headDim + i;
        float q0 = (i == dim) ? qRot0 : query[idx];
        float q1 = (i == dim + 1) ? qRot1 : query[idx + 1];
        float k0 = (i == dim) ? kRot0 : key[idx];
        float k1 = (i == dim + 1) ? kRot1 : key[idx + 1];
        score += q0 * k0 + q1 * k1;
    }
    score /= sqrt(float(headDim));
    
    // Weighted value
    float outVal = 0.0f;
    for (uint i = 0; i < seqLen; ++i) {
        uint vIdx = (i * numHeads + head) * headDim + dim;
        outVal += score * value[vIdx];
    }
    
    output[qIdx] = outVal;
}
