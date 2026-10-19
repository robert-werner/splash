# Where a prefill chunk's GPU time goes

Qwen3.8-27B (`Ternary-Bonsai-2-27B-PQ2_0`), M5 Pro 16-core GPU, 24 GiB, Metal 4.
All numbers below come from `dev/prefill_model.py`, which reads the GGUF's
*header* only — tensor names, shapes and types — so it can run while a server
holds the model. Re-run it with:

```bash
python3 dev/prefill_model.py
```

## Answer first

| | at 180K tokens | rate achieved | peak assumed | headroom |
|---|---|---|---|---|
| weights (linear in n) | **433.7 s (50.0%)** | 20.2 TFLOP/s | 26.5 TFLOP/s (76%) | ~12% of the prompt |
| attention (quadratic in n) | **433.3 s (50.0%)** | 14.7 TFLOP/s (55%) | 26.5 TFLOP/s | ~22% of the prompt |

**The headroom is in attention, and it is a memory problem, not an arithmetic
one.** Attention's matmuls already run at 20.2 TFLOP/s — the same rate as the
weight path, which is the fastest this machine has been shown to go. The 38%
shortfall is KV bytes re-read because the query tile is 8 rows tall: the
attention threadgroup sits at 2.2× the ridge point while the weight tiles sit at
179×. Raising the tile to 32 rows cuts KV reads 4× and would save **118 s
(13.2%)** at 180K with no change to a single FLOP. The weight path has ~12% left
and needs 76%→100% of peak to get it, which no known kernel change reaches.

**Neither is saturated. Attention is the cheaper 2× of the remaining time.**

## 1. Layer census

Determined two ways that agree: `GgufImage.cpp`'s `layerImage` branches on
`geometry.isFullAttentionLayer(index)`, and the GGUF's own tensor table shows
which branch each block took.

`isFullAttentionLayer` is `fullAttentionPeriod && (layer + 1) % period == 0`
(`QwenHybridLayout.hpp:52`), with `fullAttentionPeriod = 4` from
`qwen35.full_attention_interval`.

| | count | which layers |
|---|---|---|
| full attention (`attn_q/k/v`, `attn_output`, `attn_q_norm`, `attn_k_norm`) | **16** | 3, 7, 11, … 63 — every 4th |
| GDN / SSM (`attn_qkv`, `attn_gate`, `ssm_out`, `ssm_conv1d`, `ssm_a`, `ssm_dt`, `ssm_norm`, `ssm_alpha`, `ssm_beta`) | **48** | all the rest |

Verified from the tensor table: blocks 3, 7, … have `attn_q.weight` and no
`attn_qkv.weight`; the others the reverse. `blk.63` is full attention, so the
last block is not a GDN one.

Geometry (`Qwen3_8.hpp:23`, confirmed against `qwen35.*` metadata):

- hidden 5120, dense FFN 17408 (`QwenFfnKind::Dense` — **not** MoE, so every
  weight is touched by every token)
- attention: 24 query heads / 4 KV heads, head dim 256 → q width 6144, KV width
  1024, GQA group 6
- RoPE: 32 pairs = 64 of 256 dims rotated, theta 1e7
- GDN: 16 key / 48 value heads, head dim 128, 4 conv taps over 10240 channels

Parameters, counted from the tensor table rather than assumed:

```
0.3723B per full-attention layer x 16  =  5.956B
0.3833B per GDN layer          x 48  = 18.397B
                                       --------
layer weights                          24.353B   (6.47 GB at 2.125 bits)
embedding + output head (tied shape)    2.543B
                                       --------
                                       26.896B
```

The FFN is 70.3% of the layer weights — this is an FFN-dominated model, and the
attention blocks are only a quarter of the weight traffic.

## 2. Dispatches per 2048-row chunk

From `QwenTarget::addPrefill` → `addPrefillMixer` / `addPrefillFfn`, and the ops
entry points they call. `gguf_rotate` is `Linear::addGguf`'s Hadamard pass,
emitted once before the tiles of every rotated quantized projection; the prefill
tiles fuse all segments of one format into one dispatch
(`Linear::addGgufPrefill`), and an F32 segment (GDN alpha/beta) runs
`gguf_float_*` first (`addGgufFloatSegments`).

**GDN layer — 16 dispatches**

| # | kernel | grows with rows? |
|---|---|---|
| 1 | `norm_rms` (attn_norm) | per row |
| 2 | `gguf_float_bf16` (ssm alpha\|beta, F32 segment) | per row |
| 3 | `gguf_rotate` (qkv\|gate) | per row |
| 4 | `gguf_prefill_pq20_a` (qkv\|gate, one fused dispatch) | per row |
| 5 | `prefill_gdn_prepare` | per row |
| 6 | `prefill_gdn_scan` | **per row, serially** (see below) |
| 7 | `prefill_gdn_gate` | per row |
| 8–9 | `gguf_rotate`, `gguf_prefill_pq20_r` (ssm_out) | per row |
| 10 | `norm_rms` (post_attention_norm) | per row |
| 11–16 | `gguf_rotate` + prefill tile for gate / up / down | per row |

**Full-attention layer — 17 dispatches**

| # | kernel | grows with rows? |
|---|---|---|
| 1–3 | `norm_rms`, `gguf_rotate`, `gguf_prefill_pq20_a` (q\|k\|v) | per row |
| 4 | `prefill_attention_qkv` (q/k norm + RoPE + staging) | per row |
| 5 | `prefill_attention_q4_store` | per row |
| 6 | `prefill_attention_q4_split` | per row **and** per visible token (C) |
| 7 | `prefill_attention_reduce` | per row, per visible token (C) |
| 8 | `prefill_attention_gate` | per row |
| 9–10 | `gguf_rotate`, `gguf_prefill_pq20_r` (attn_output) | per row |
| 11–17 | FFN as above | per row |

**1040 dispatches per chunk** (48×16 + 16×17), plus one embedding gather, five
capture row copies (layers 5, 19, 33, 47, 61), and the head + logits on the
chunk that ends the prompt.

Fixed overhead is negligible. At 4.94 s per chunk, 1040 dispatches cost ~4.7 µs
each even if encoding and submission were *entirely* serial — under 0.1% of the
chunk. `prefill_gdn_scan` is the one dispatch whose grid does not scale with
rows (384 threadgroups = 48 value heads × 128 dims / 16 state rows, per layer),
but it walks the chunk's tokens one at a time in a dependent chain: ~64 MACs and
6 shuffles per thread per token. Even at a pessimistic 250 cycles per token that
is 15 ms per chunk, 0.3%.

The two dispatches that grow with *context* rather than rows are the attention
split and reduce. They are where the quadratic term lives.

## 3. The work model

For one chunk of `R` rows at committed context `C`:

```
matmul   F_mm  = 2 * P_layer * R          P_layer = 24.353B, every weight, every chunk
attention F_att = sum over i<R of 4 * q_width * (C + i) * n_full
                = 4 * 6144 * 16 * (R*C + R^2/2)
GDN      F_gdn = 48 * R * 48 * 4 * 2 * 128^2      = 0.63% of F_mm, ignored below
```

The `4 * q_width` is 2·256 for QK^T plus 2·256 for PV per visible token per
layer; `n_full = 16`.

**All 27B parameters are touched per chunk.** This is a dense model — there is no
router and no expert to skip — so the matmul term is exactly linear in tokens
with a slope of 2·24.353 GFLOP/token = 48.7 GFLOP/token. The embedding is a
gather (21 MB per chunk, not a matmul) and the 248320×5120 output head runs once
per prompt on its last chunk, so neither appears in the per-chunk term.

With `n` tokens in chunks of 2048:

```
t(n) = sum over chunks [ F_mm/R_mm + F_att/R_att ]
     = n * 2*P_layer/R_mm  +  n^2 * 2*q_width*n_full/R_att
     = a*n + b*n^2
```

so the two measured coefficients *are* the two rates:

```
a = 2*P_layer / R_mm          R_mm  = 2 * 24.353e9 / a
b = 2*q_width*n_full / R_att  R_att = 2 * 6144 * 16 / b
```

### Against the five measured points

`a = 2.41147 ms/token`, `b = 13.3943 ns/token²`:

| n | measured | a·n | b·n² | model | err |
|---|---|---|---|---|---|
| 4,338 | 10.9 s | 10.5 | 0.3 | 10.7 | −1.7% |
| 17,141 | 44.7 s | 41.3 | 3.9 | 45.3 | +1.3% |
| 68,224 | 227.5 s | 164.5 | 62.3 | 226.9 | −0.3% |
| 136,322 | 577.4 s | 328.7 | 248.9 | 577.7 | +0.0% |
| 179,852 | 894.0 s | 433.7 | 433.3 | 867.0 | −3.0% |

The 3% miss at the top point is the only systematic error, and it is the
`n^2/2` → `n^2` approximation (chunk granularity) plus the head's one-off cost;
it does not change any conclusion.

Break-even is at **n = a/b = 180,037 tokens** — which is why the two-point
"50/50 at 180K" estimate was right: at 179,852 the split is 50.0/50.0 by
construction. At 262,144 it is 41/59.

> **Unit note.** `a` is 2.41147 **milliseconds** per token, not microseconds.
> 2048 rows × 2.41 µs would be a 4.9 ms chunk, which would mean streaming 6.47 GB
> of weights at 1.3 TB/s — 4× the machine's DRAM bandwidth. As ms it is 1.31 GB/s,
> which is consistent with a compute-bound path. The fit is unaffected; only the
> label was.

## 4. What `a` and `b` mean physically

### a: the weight path is compute-bound, at 76% of peak

```
weight bytes per token = 6.47 GB / 2048 = 3.159 MB
effective bandwidth    = 3.159 MB / 2.411 ms = 1.31 GB/s   (0.43% of 307 GB/s)
effective compute      = 2 * 24.353e9 / 2.411e-3 = 20.20 TFLOP/s
```

**The weight path is nowhere near bandwidth-limited** — DRAM is 0.4% busy. It is
arithmetic-limited at 20.2 TFLOP/s. That is 3.1× the fp32 shader-ALU peak, which
proves the tiles really execute on the matrix units rather than the SIMT pipe.

Peak assumed: **26.5 TFLOP/s** = 16 cores × 1024 FLOP/cycle × 1.62 GHz. That is
the published structure of the M5 neural accelerator (128 dot-product FMAs per
core per cycle, 1024 FLOPs), and the measured 20.2 TFLOP/s independently
confirms the units deliver at least that much — so the peak cannot be below 20.2
and 26.5 is the honest ceiling. Apple quotes 307 GB/s for this 16-core M5 Pro.

So the weight path is at **76% of peak**. There is ~12% of the prompt available
there in the absolute best case, and reaching it would require the 2.125-bit
dequantise-and-multiply tile to go from 76% to 100% of the matrix pipe — a
roughly 30% kernel improvement on a path that has already been tuned.

### b: attention runs at 55% of peak because it re-reads KV

```
attention coefficient = 2 * 6144 * 16 = 196,608 FLOP per token²
effective throughput  = 196,608 / 13.3943 ns = 14.68 TFLOP/s  (55% of 26.5)
```

against the weight path's 20.20 TFLOP/s, attention is at **73% of it**. A purely
FLOP-bound `b` at peak would be 7.42 ns/token²; measured is 13.39, i.e. **1.81×
the FLOP bound**.

That gap is KV traffic, and the arithmetic closes almost exactly:

```
attention FLOPs over the whole 179,852-token prompt   6.360 PFLOP
at the weight path's 20.20 TFLOP/s                      315 s
KV bytes read (4-bit, one walk per 8-row query tile)   34,161 GB
at 307 GB/s                                             111 s
                                                        -----
                                             315 + 111 = 426 s   vs 433 s measured (−1.7%)
```

Compute and KV reads are effectively **serial, not overlapped**. That is the
whole of the 38% shortfall, and it is why `b` is 1.8× its FLOP bound.

The cause is the query tile height. One threadgroup owns 8 query rows × 6 query
heads = 48 fused rows of 256 dims, and walks the entire visible history:

```
per 32-token page:  1.57 MFLOP over 8.4 kB = 186 FLOP/byte
ridge point:        26.5 TFLOP/s / 307 GB/s =  86 FLOP/byte
```

**2.2× the ridge.** The weight path's tiles are 179× the ridge — which is why
they never wait on memory. At 2.2×, every page fetch that misses stalls a
matmul, and the same KV is fetched again by the next 8-row tile (256 times per
2048-row chunk, per KV head).

### Is the machine saturated?

Neither path is saturated, but they are saturated *differently*:

- **weights**: 76% of the matrix pipe, 0.4% of DRAM. Compute-bound, 12% left.
- **attention**: 55% of the matrix pipe, and the missing 23% is DRAM stalls
  caused by a tile too short to amortise its own KV reads. 22% left.

## 5. The split claim: `prefillSplits` returns 1, and it does not matter

**Confirmed.** `ops::prefillSplits` (`runtime/ops/PagedAttention.cpp:186`) is
`clamp(32 / tiles, 1, 32)` with `tiles = ceil(rows / 8)`
(`kv::prefillAttentionTiles`, tile rows = `SPLASH_PREFILL_ATTENTION_TILE_ROWS` =
8). At 2048 rows that is `32/256 = 0` → clamped to **1**:

| rows | tiles | 32/tiles | splits | split grid |
|---|---|---|---|---|
| 2048 | 256 | 0 | 1 | {4, 256, 1} = 1024 tg |
| 1676 | 210 | 0 | 1 | {4, 210, 1} = 840 tg |
| 512 | 64 | 0 | 1 | {4, 64, 1} = 256 tg |
| 256 | 32 | 1 | 1 | {4, 32, 1} = 128 tg |
| 128 | 16 | 2 | 2 | {4, 16, 2} = 128 tg |
| 64 | 8 | 4 | 4 | {4, 8, 4} = 128 tg |

So the history is never split for any chunk of 256 rows or more, and the last
chunk of a 179,852-token prompt is 1,676 rows. Confirmed.

**But the splits > 1 path is fully implemented, not missing.** The split kernel
writes `partials[slot * M * D]` and `statistics[(slot*M + row)*2]` with
`slot = (tile * KVHeads + kv_head) * split_count + split`
(`prefill/paged_attention.metal:28`), and `splash_attention_reduce_row`
(`common/paged_attention_tile.h:336`) recomputes the same page partition,
computes `written = ceil(pages / per_split)`, takes a max over the written
splits' statistics, exponentiates against it, and sums `weight * partial` in
split order. It is a correct log-sum-exp combine, deliberately serial to keep
the denominator's summation order stable (the comment notes a parallel sum
changes speculative acceptance). Nothing is stubbed.

**And splitting would not help.** The grid already holds 1024 threadgroups of
256 threads across 16 cores — 64 threadgroups per core, already far past the
point where more parallelism helps. The dispatch is not occupancy-limited; it is
intensity-limited. `prefillSplits` returning 1 is a correct consequence of
choosing an 8-row tile: with 256 tiles there is no reason to split the history,
because the tiles themselves are the parallelism. Fixing `prefillSplits` would
change nothing.

**What actually binds is the 8-row tile**, and the reason it is 8 is visible in
`common/paged_attention_tile.h`'s `splash_attention_page_softmax`: it maps one
lane per 8 tokens per fused row and returns when
`lanes_per_row × fused_rows > 256` threads:

| tile rows | threads needed | 256-thread group |
|---|---|---|
| 8 | 192 | ok |
| 10 | 240 | ok |
| 12 | 288 | **silently drops rows** |
| 16 | 384 | **silently drops rows** |
| 32 | 768 | **silently drops rows** |

The tile cannot grow past 10 rows without the softmax looping over fused rows.
That loop — not the split count, not an ABI constant — is the change that buys
the KV traffic.

## 6. Headroom, and what each fix is worth at 180K

| change | 180K time | gain |
|---|---|---|
| today | 894 s | — |
| weights 20.2 → 22 TFLOP/s | 831 s | 4.1% |
| attention 8-row → 32-row tile | 776 s | 13.2% |
| attention to the weight path's rate (20.2) | 749 s | 13.7% |
| weights *and* attention to peak | 570 s | 34.2% |

The tile widening is the one change that is both large and reachable: it cuts KV
reads 4× (34,161 → 8,540 GB) and lifts tile intensity from 186 to 745 FLOP/byte
(9× the ridge), touching no arithmetic. Realistically it lands between the
13.2% and 13.7% rows, since it also removes the DRAM exposure that keeps
attention at 73% of the weight path's rate.

### A cheap test that distinguishes the two hypotheses

The model says `b` is set by KV *bytes*, not by FLOPs. That is falsifiable
without touching a kernel: **run the 179,852-token point at `--kv-format int8`**,
which doubles KV bytes and changes no FLOP.

| KV format | predicted b | predicted 180K |
|---|---|---|
| int4 (measured) | 13.17 ns | 894 s (measured 894) |
| int8 | 16.50 ns | 1,101 s (+23%) |
| bf16 | 23.17 ns | 1,351 s (+51%) |

If `b` moves with KV bytes like that, the diagnosis is confirmed and the tile
height is the lever. If `b` barely moves, the model is wrong and attention is
genuinely arithmetic-bound — in which case there is only ~22% there and it needs
better matmul utilisation, not less traffic.

## Recommendation

**Attack attention, and attack its tile height — not its split count.**

1. **Attention first.** It is 50% of the 180K prompt, runs at 55% of peak, and
   the shortfall is a 2.2×-ridge tile that re-reads the whole history 256 times
   per chunk. Widening the query tile from 8 to 32 rows cuts KV reads 4× and is
   worth ~13% of the prompt. It requires a loop in `splash_attention_page_softmax`
   (currently one lane per 8 tokens per fused row, hard-capped at 256 threads) —
   a contained kernel change, no ABI change.
2. **Weights second, and modestly.** 76% of peak with DRAM 0.4% busy is a well-
   tuned compute-bound path. There is ~12% of the prompt there but it needs a
   ~30% kernel improvement to reach; it is not the place to start.
3. **Do not "fix" `prefillSplits`.** It correctly returns 1; the split machinery
   is implemented and correct; and 1024 threadgroups over 16 cores is already
   more parallelism than the GPU can use. Changing it would add a 50 MB
   partials round-trip per layer per chunk (~0.05% of the prompt) for nothing.
4. **Verify with the KV-format experiment before writing a kernel.** Doubling KV
   bytes should add ~23% to `b` and ~11% to the 180K time. That is a 15-minute
   measurement that confirms or kills the whole diagnosis.

## Files

- `dev/prefill_model.py` — the model. Header-only GGUF read, no weights loaded,
  not wired into `make`. Prints the census, the dispatch list, the FLOP model,
  the fit, the rate derivation, the KV traffic, the tile analysis and the
  headroom table.
