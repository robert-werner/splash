# Prefill scaling — measured model

**Date**: 2026-10-19
**Hardware**: M5 Pro, 24 GiB unified memory, macOS 27.0.1
**Build**: main `ae667a6d`, `build/splash` 2.6 MB, `-Werror` clean
**Config**: `--kv-format int4`, context 262,144, Python server, greedy, `max_tokens=8`

## Measurements

Four prompt lengths, synthetic filler text, one request each, no prefix reuse:

| prompt tokens | wall | rate |
|---:|---:|---:|
| 4,338 | 10.9 s | 399.6 tok/s |
| 17,141 | 44.7 s | 383.2 tok/s |
| 68,224 | 227.5 s | 299.9 tok/s |
| 136,322 | 577.4 s | 236.1 tok/s |

Two earlier points, different prompt content, same config:

| prompt tokens | wall | rate |
|---:|---:|---:|
| 20,750 | 56.8 s | 365 tok/s |
| 179,852 | 894 s | 201 tok/s |

## The model

A two-term fit `t(n) = a·n + b·n²` over the four points reproduces all of them:

```
a = 2.41147 ms/token        (linear: weights, compute-bound)
b = 13.3943 ns/token^2      (quadratic: attention over history)
```

| n | measured | model | error |
|---:|---:|---:|---:|
| 4,338 | 10.9 s | 10.7 s | −1.7% |
| 17,141 | 44.7 s | 45.3 s | +1.3% |
| 68,224 | 227.5 s | 226.9 s | −0.3% |
| 136,322 | 577.4 s | 577.7 s | +0.0% |
| 179,852 (holdout) | 894 s | 867 s | −3.0% |

The 180K point was not used in the fit and lands within 3%, so the model holds
across a 40x range of prompt length.

**Break-even at n = 180,036 tokens**, where the two terms are equal. The
pure-linear asymptote is **415 tok/s**.

### Correction: the linear coefficient is milliseconds, not microseconds

An earlier revision of this note labelled `a` as 2.41147 **us**/token. That is
wrong by 1000x: the fit is in seconds and tokens, so `a` is **2.41147 ms/token**.
The distinction matters, because the two readings imply opposite diagnoses:

- At 2.41 µs/token the weight path would have to stream 3.159 MB of weights per
  token at **1.3 TB/s**, four times this machine's DRAM bandwidth — impossible.
- At 2.41 ms/token it is **1.31 GB/s** (DRAM 0.4% busy) and
  `2 × 24.353e9 / 2.411e-3` = **20.2 TFLOP/s**, i.e. **76% of a 26.5 TFLOP/s
  peak**. The weight path is compute-bound and nearly saturated, not
  bandwidth-bound.

The fit itself is unchanged; only the label was wrong. See `PREFILL_MODEL.md`
for the independent FLOP census that confirms the compute-bound reading.

## What the split means

| n | predicted | linear share | quadratic share |
|---:|---:|---:|---:|
| 68K | 227 s | 73% | 27% |
| 180K | 867 s | 50% | 50% |
| 262K | 1,553 s | 41% | 59% |

At the model's maximum context, **59% of prefill time is attention over
history**, which does not care about the row budget at all. Raising
`SPLASH_PREFILL_TOKEN_BUDGET` from 2048 to 4096 attacks only the linear term,
and even eliminating every per-chunk overhead entirely cannot beat 415 tok/s.

## Why the quadratic term is there — a concrete defect

`PagedAttention::prefillPlan` sizes the history parallelism as

```cpp
uint32_t prefillSplits(uint32_t tiles) {           // PagedAttention.cpp:186
  constexpr uint32_t maximum = SPLASH_PREFILL_ATTENTION_MAXIMUM_SPLITS;  // 32
  return std::clamp(maximum / tiles, 1U, maximum);
}
```

with `tiles = prefillAttentionTiles(rows) = rows / 8`
(`SPLASH_PREFILL_ATTENTION_TILE_ROWS = 8`). For the default 2048-row chunk that
is `tiles = 256`, so `32 / 256 = 0` and the clamp returns **1** — for *every*
chunk size, since `tiles >= 256` always. The dispatch is
`{kvHeads, tiles, splits} = {4, 256, 1}`: 1,024 threadgroups, **none of which
split the history**. Each query tile walks the whole committed prefix alone.

So attention work per chunk grows linearly in `n`, and the total over
`n/2048` chunks grows as `n²` — exactly the measured `b·n²`. The `splits`
machinery exists (partials, statistics, a reduce kernel) but is effectively
disabled by the integer division.

Raising `SPLASH_PREFILL_ATTENTION_MAXIMUM_SPLITS` alone does **not** fix it:
`maximum / 256 >= 2` needs `maximum >= 512`, and the workspace is
`tiles · splits · kvHeads · fusedRows`, which then grows 16x. The tile size is
the thing to change: with `SPLASH_PREFILL_ATTENTION_TILE_ROWS = 32`, `tiles = 64`
and `splits = 32/64 → 1` still; with 64, `tiles = 32` and `splits = 1`; the
ratio only frees up when `tiles <= 32`, i.e. tile rows >= 64. That trade needs
its own measurement — larger tiles mean fewer, fatter threadgroups.

**This is the single highest-value lead in the engine**: it is the only
remaining lever whose payoff *grows* with context length, and it is a
scheduling constant, not a rewrite.

## Recommendation

1. **Do not spend the effort on the row budget alone.** It is worth at most the
   linear half, and only the per-chunk overhead part of it.
2. **Measure the tile/splits trade** (`SPLASH_PREFILL_ATTENTION_TILE_ROWS` 8 vs
   16/32/64 against `SPLASH_PREFILL_ATTENTION_MAXIMUM_SPLITS`), watching both
   prefill rate at 180K and the attention workspace bytes. That is where the
   quadratic half can actually be attacked.
3. Keep the model above as the acceptance yardstick: any change should move
   `a` or `b`, and the two coefficients say which.

## Reproducing

```bash
TRANSFORMERS_OFFLINE=1 HF_HUB_OFFLINE=1 .venv/bin/python -m server.server \
  install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6 \
  --tokenizer install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6/tokenizer \
  --model prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0 --kv-format int4 --port 8060
```

Then one request per prompt length with `max_tokens=8`, temperature 0, unique
filler text so nothing hits the prefix cache. Read `prompt_tokens` from
`usage`; the wall time is the whole request, so keep `max_tokens` small.

---
**Generated by**: the model 5.5

## Falsification test: the attention bottleneck is NOT KV bytes

`PREFILL_MODEL.md` proposed a cheap test of its own diagnosis: the model claimed
attention stalls on KV reads, so doubling the KV bytes without changing any
FLOPs should raise `b` from 13.17 to 16.50 ns/token² and cost **+23%** at 180K.

Measured on the same build, same prompt generator, same seeds, `--kv-format int8`
instead of int4 (2x the KV bytes, identical FLOPs):

| prompt | int4 | int8 | delta |
|---:|---:|---:|---:|
| ~17K | 383.5 tok/s | 384.3 tok/s | +0.2% |
| ~68K | 299.9 tok/s | 295.2 tok/s | −1.5% |
| ~136K | 236.1 tok/s | 230.9 tok/s | −2.2% |

Fitting `b` with `a` held fixed (the weight path does not touch KV):

| int8 point | b (ns/token²) |
|---:|---:|
| 17,908 | 10.65 |
| 71,007 | 13.74 |
| 141,783 | 13.54 |

**int8 b ≈ 12.64 ns/token² vs int4 13.39 — a ratio of 0.94, not 1.25.** Doubling
the KV bytes made prefill *very slightly faster*, i.e. within run-to-run noise.

**Conclusion: the attention path is not KV-bandwidth-bound.** The "compute and KV
reads are serial" diagnosis in `PREFILL_MODEL.md` is refuted, and with it the
recommendation to widen the query tile 8→32 rows *in order to cut KV reads 4x*
for a 13.2% gain. That change would remove a cost that is not being paid.

What survives: attention runs at ~14.7 TFLOP/s, 55% of the 26.5 TFLOP/s peak the
weight path reaches 76% of. So attention is *compute*-inefficient, not
memory-starved — consistent with a skinny 8-row GEMM that under-fills the matrix
units, and with the softmax kernel's 256-thread cap (the reason 12 rows is not
reachable today). Widening the tile may still help, but for arithmetic-intensity
reasons, and that must be shown by measuring FLOP utilisation, not KV traffic.

**Method note:** this is why the model was written with a falsifiable prediction
before any kernel work. One measurement cost ~20 minutes and cancelled a change
that would have been days of work on a false premise.
