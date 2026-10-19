# Hybrid GGUF Baseline Decode Throughput

**Test Configuration:**
- Model: Hybrid GGUF (163,833 context int8 KV)
- Server: http://127.0.0.1:8080
- Mode: GPU-only (ANE FFN split disabled due to rotated basis)
- Test: 4.3K-token sensor log prompt (random.seed(3)) + 200 token generation

**Primary Results (Large Context):**
- **Actual throughput:** 25.3 tok/s
- **Baseline:** 62.9 tok/s
- **Delta:** -59.8%
- **Phase 2 target:** 75.0–82.0 tok/s (+20–30%)
- **Status:** FAIL

**Measurements:**
- Prompt tokens: 14,580
- Decode tokens: 199
- Wall time: 48.6s
- TTFT: 40,671ms
- Decode time: 7,865ms
- Finish reason: length

**Speculation Metrics:**
- Drafted tokens: 630
- Accepted draft tokens: 110
- Cycles: 90
- Accept rate: 17.5%

**Context Size Impact:**
- Large context (14.5K tokens): 25.3 tok/s
- Small context (13 tokens): 47.5 tok/s
- Context overhead: -46.8%

**Analysis:**
- GPU-only decode is running 60% below baseline with large context
- Small context shows 25% below baseline, indicating context size sensitivity
- Both measurements significantly below Phase 2 target range
- Large prefill time (40.6s for 14.5K tokens) suggests GPU saturation or memory bandwidth bottleneck

**Notes:**
- Rotated basis in PTQ1_0 blocks ANE FFN split (views of projection planes require unrotated quantized tensor)
- This measurement establishes GPU-only baseline before ANE path is fixed
- Gap to target lower bound: +49.7 tok/s (from 25.3 to 75.0)
- Performance is significantly worse than expected — may indicate regression or test environment issue

**Generated:** 2026-10-19 16:30:12
