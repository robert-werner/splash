#include <metal_stdlib>
using namespace metal;

// Int4 KV quantization kernel
kernel void quantizeKvInt4(
    device const float *input [[buffer(0)]],
    device uchar *output [[buffer(1)]],
    device float *scale [[buffer(2)]],
    constant uint &count [[buffer(3)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= count / 2) return;
    
    float val0 = input[id * 2];
    float val1 = input[id * 2 + 1];
    float maxVal = max(abs(val0), abs(val1));
    float s = maxVal / 7.0f;
    scale[id] = s;
    
    int q0 = int(round(val0 / s));
    int q1 = int(round(val1 / s));
    q0 = clamp(q0, -7, 7);
    q1 = clamp(q1, -7, 7);
    
    output[id] = uchar((q0 & 0xF) | ((q1 & 0xF) << 4));
}

kernel void dequantizeKvInt4(
    device const uchar *input [[buffer(0)]],
    device const float *scale [[buffer(1)]],
    device float *output [[buffer(2)]],
    constant uint &count [[buffer(3)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= count / 2) return;
    
    uchar packed = input[id];
    int q0 = int(packed & 0xF);
    int q1 = int((packed >> 4) & 0xF);
    if (q0 > 7) q0 -= 16;
    if (q1 > 7) q1 -= 16;
    
    float s = scale[id];
    output[id * 2] = float(q0) * s;
    output[id * 2 + 1] = float(q1) * s;
}
