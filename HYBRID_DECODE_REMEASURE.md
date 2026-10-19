# Hybrid Decode Throughput Remeasurement

**Date:** 2026-10-19  
**Environment:** M5 Pro 24GB, macOS 27.0.1  
**Server:** http://127.0.0.1:8080  
**Model:** prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0 (hybrid-real)

## Test Configuration

- **Sensor log generator:** `random.seed(3)`, 550 lines
- **Input tokens:** ~14,354 (target: 14.5K)
- **Output tokens:** 200
- **Temperature:** 0
- **Method:** POST /v1/chat/completions

## Results

### Individual Measurements (uncached)

| Run | Seed | Input Tokens | Output Tokens | Decode tok/s | Time (s) |
|-----|------|--------------|---------------|--------------|----------|
| 1   | 3    | 14,354       | 200           | **23.9**     | 46.43    |
| 2   | 42   | 14,348       | 200           | **26.2**     | 47.25    |

### Aggregate Metrics

- **Average decode throughput:** **25.06 tok/s** (from /status endpoint)
- **Baseline (PQ2_0 alone):** 62.9 tok/s
- **Previous hybrid result:** 25.3 tok/s

## Performance Analysis

```
Actual:   25.06 tok/s
Baseline: 62.9 tok/s
Delta:    -37.84 tok/s (-60.2%)
Target:   56.61 tok/s (90% of baseline)
```

## Verdict

**❌ FAIL**

The hybrid quantization scheme achieves only **39.8% of baseline performance**, falling far short of the 90% target (56.61 tok/s).

### Comparison with Previous Measurement

- **Previous hybrid:** 25.3 tok/s
- **Current hybrid:** 25.06 tok/s
- **Difference:** -0.24 tok/s (-0.9%)

**The fix did not improve decode throughput.** Performance remains nearly identical to the pre-fix measurement.

## Technical Details

### Model Configuration
- **Weights:** `install/models/hybrid-real/`
- **Tokenizer:** majentik/Qwen3.8-27B-MLX-2bit (EOS token: 248044)
- **Format:** hybrid PTQ1+PQ2 (rotation_signs.bin + quantized weights)

### Server Logs
```
19:21:09 Done · input 14,354 · cached 0 · output 200 · TTFT 38.1s · 23.9 tok/s · 2.1 tok/cycle
19:22:11 Done · input 14,348 · cached 0 · deduplicated 32 · output 200 · TTFT 39.6s · 26.2 tok/s · 2.2 tok/cycle
```

### /status Endpoint Extract
```json
{
  "decode_output_tokens": 600,
  "decode_wall_ms": 23942.74896,
  "decode_cycle_ms": 24063.51112,
  "decode_tokens_per_second": 25.0597791,
  "drafted_tokens": 1967,
  "accepted_draft_tokens": 316,
  "draft_acceptance_rate": 0.1606507372
}
```

## Conclusion

The hybrid quantization approach (PTQ1 attention + PQ2 FFN) shows **severe performance degradation** compared to pure PQ2_0 baseline. The 60% throughput loss makes this configuration unsuitable for production use unless:

1. Further optimization reduces the hybrid overhead
2. The quality improvement justifies the 2.5× slowdown
3. Alternative hybrid splits (e.g., different layer allocation) perform better

The fix applied did not address the fundamental performance bottleneck.
