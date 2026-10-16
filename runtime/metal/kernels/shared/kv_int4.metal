#include <metal_stdlib>
using namespace metal;

// INT4 KV quantization: 2 values per byte, 4-bit signed integers [-8, 7]
// Symmetric quantization with per-head scales and zero points

// Quantize BF16 KV to INT4 format (2 values per byte)
kernel void quantize_kv_int4(
    device const bfloat *input [[buffer(0)]],       // [tokens, heads, dim]
    device uint8_t *output [[buffer(1)]],            // [tokens, heads, dim/2] packed
    device float *scales [[buffer(2)]],              // [tokens, heads]
    device float *zeros [[buffer(3)]],               // [tokens, heads]
    constant uint32_t &heads [[buffer(4)]],
    constant uint32_t &dim [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]]            // (token, head)
) {
    const uint32_t token = gid.x;
    const uint32_t head = gid.y;
    const uint32_t base = (token * heads + head) * dim;
    
    // Find min/max for this head
    float minVal = INFINITY;
    float maxVal = -INFINITY;
    
    for (uint32_t i = 0; i < dim; ++i) {
        float val = float(input[base + i]);
        minVal = min(minVal, val);
        maxVal = max(maxVal, val);
    }
    
    // Calculate scale and zero point for symmetric quantization
    // Range: [-8, 7] (4-bit signed)
    const float range = maxVal - minVal;
    const float scale = range > 0.0f ? range / 15.0f : 1.0f;
    const float zero = -8.0f - minVal / scale;
    
    // Store scale and zero point
    const uint32_t scaleIdx = token * heads + head;
    scales[scaleIdx] = scale;
    zeros[scaleIdx] = zero;
    
    // Quantize and pack 2 values per byte
    const uint32_t outBase = (token * heads + head) * (dim / 2);
    
    for (uint32_t i = 0; i < dim; i += 2) {
        float val0 = float(input[base + i]);
        float val1 = float(input[base + i + 1]);
        
        // Quantize to 4-bit signed integers
        int q0 = clamp(int(round(val0 / scale + zero)), -8, 7);
        int q1 = clamp(int(round(val1 / scale + zero)), -8, 7);
        
        // Pack into byte: high 4 bits = q0, low 4 bits = q1
        uint8_t packed = uint8_t(((q0 & 0xF) << 4) | (q1 & 0xF));
        output[outBase + i / 2] = packed;
    }
}

// Dequantize INT4 KV to BF16 format
kernel void dequantize_kv_int4(
    device const uint8_t *input [[buffer(0)]],       // [tokens, heads, dim/2] packed
    device const float *scales [[buffer(1)]],        // [tokens, heads]
    device const float *zeros [[buffer(2)]],         // [tokens, heads]
    device bfloat *output [[buffer(3)]],             // [tokens, heads, dim]
    constant uint32_t &heads [[buffer(4)]],
    constant uint32_t &dim [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]]            // (token, head)
) {
    const uint32_t token = gid.x;
    const uint32_t head = gid.y;
    
    // Load scale and zero point
    const uint32_t scaleIdx = token * heads + head;
    const float scale = scales[scaleIdx];
    const float zero = zeros[scaleIdx];
    
    const uint32_t inBase = (token * heads + head) * (dim / 2);
    const uint32_t outBase = (token * heads + head) * dim;
    
    // Unpack and dequantize
    for (uint32_t i = 0; i < dim / 2; ++i) {
        uint8_t packed = input[inBase + i];
        
        // Unpack: high 4 bits = val0, low 4 bits = val1
        int q0 = int(int8_t(packed & 0xF0)) >> 4;  // Sign-extend from 4-bit
        int q1 = int(int8_t((packed & 0x0F) << 4)) >> 4;
        
        // Dequantize
        float val0 = (float(q0) - zero) * scale;
        float val1 = (float(q1) - zero) * scale;
        
        output[outBase + i * 2] = bfloat(val0);
        output[outBase + i * 2 + 1] = bfloat(val1);
    }
}

// Fast INT4 attention kernel: compute attention with quantized KV
// This kernel reads INT4 K/V, dequantizes on-the-fly, and computes QK^T and PV
kernel void int4_paged_attention(
    device const bfloat *queries [[buffer(0)]],         // [batch, heads, dim]
    device const uint8_t *keys_int4 [[buffer(1)]],      // [pages, heads, page_size, dim/2]
    device const uint8_t *values_int4 [[buffer(2)]],    // [pages, heads, page_size, dim/2]
    device const float *key_scales [[buffer(3)]],       // [pages, heads, page_size]
    device const float *key_zeros [[buffer(4)]],        // [pages, heads, page_size]
    device const float *value_scales [[buffer(5)]],     // [pages, heads, page_size]
    device const float *value_zeros [[buffer(6)]],      // [pages, heads, page_size]
    device const uint32_t *page_table [[buffer(7)]],    // [batch, max_pages]
    device bfloat *output [[buffer(8)]],                // [batch, heads, dim]
    constant uint32_t &batch [[buffer(9)]],
    constant uint32_t &heads [[buffer(10)]],
    constant uint32_t &dim [[buffer(11)]],
    constant uint32_t &seq_len [[buffer(12)]],
    constant uint32_t &page_size [[buffer(13)]],
    uint3 gid [[thread_position_in_grid]]               // (batch_idx, head, dim_idx)
) {
    const uint32_t b = gid.x;
    const uint32_t h = gid.y;
    const uint32_t d = gid.z;
    
    if (b >= batch || h >= heads || d >= dim) return;
    
    // Load query
    const float query_val = float(queries[b * heads * dim + h * dim + d]);
    
    // Compute attention scores and weighted sum
    float sum = 0.0f;
    float max_score = -INFINITY;
    float sum_exp = 0.0f;
    
    // First pass: compute scores and find max
    float scores[256];  // Assuming seq_len <= 256 for this kernel
    uint32_t token_count = min(seq_len, uint32_t(256));
    
    for (uint32_t t = 0; t < token_count; ++t) {
        const uint32_t page_idx = t / page_size;
        const uint32_t page_offset = t % page_size;
        const uint32_t page = page_table[b * ((seq_len + page_size - 1) / page_size) + page_idx];
        
        // Dequantize key value
        const uint32_t key_base = (page * heads * page_size + h * page_size + page_offset) * (dim / 2);
        const uint32_t scale_idx = page * heads * page_size + h * page_size + page_offset;
        
        float key_val = 0.0f;
        if (d < dim) {
            const uint8_t packed = keys_int4[key_base + d / 2];
            const int q = (d & 1) ? int(int8_t((packed & 0x0F) << 4)) >> 4 
                                  : int(int8_t(packed & 0xF0)) >> 4;
            key_val = (float(q) - key_zeros[scale_idx]) * key_scales[scale_idx];
        }
        
        // Compute QK^T score
        const float score = query_val * key_val;
        scores[t] = score;
        max_score = max(max_score, score);
    }
    
    // Second pass: softmax and weighted sum
    for (uint32_t t = 0; t < token_count; ++t) {
        const float exp_score = exp(scores[t] - max_score);
        sum_exp += exp_score;
        
        const uint32_t page_idx = t / page_size;
        const uint32_t page_offset = t % page_size;
        const uint32_t page = page_table[b * ((seq_len + page_size - 1) / page_size) + page_idx];
        
        // Dequantize value
        const uint32_t value_base = (page * heads * page_size + h * page_offset) * (dim / 2);
        const uint32_t scale_idx = page * heads * page_size + h * page_size + page_offset;
        
        float value_val = 0.0f;
        if (d < dim) {
            const uint8_t packed = values_int4[value_base + d / 2];
            const int q = (d & 1) ? int(int8_t((packed & 0x0F) << 4)) >> 4
                                  : int(int8_t(packed & 0xF0)) >> 4;
            value_val = (float(q) - value_zeros[scale_idx]) * value_scales[scale_idx];
        }
        
        sum += exp_score * value_val;
    }
    
    // Normalize and write output
    const float result = sum / sum_exp;
    output[b * heads * dim + h * dim + d] = bfloat(result);
}
