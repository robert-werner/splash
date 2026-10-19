# Deep audit: what to improve on M5 Pro 24 GiB / 1 TB

**Date**: 2026-10-19 · **Build**: main `5045af8b`, `-Werror` clean, 231 CPU checks green
**Hardware**: M5 Pro, 24 GiB unified memory, 1 TB SSD, macOS 27.0.1

Everything below is measured or read out of the code in this session. Where a
number is an estimate it says so. Nothing here repeats a claim that a
measurement refuted.

---

## 0. The headline correction

**ANE is not dead — it is dead only for Prism's ternary models.**

Verified by running the ordinary unrotated GGUF
(`unsloth/Qwen3.8-27B-GGUF:Qwen3.8-27B-UD-IQ3_XXS`, 10 GB, no
`prism.hadamard.*` metadata, no `InputRotation` attached):

```
Neural Engine FFN split at share 0.50 for chunks of 512 rows or more,
1.6% from the GPU alone on the Neural Engine's part
```

The split **initialises, calibrates and runs**. The earlier conclusion
("ANE impossible") is true only for PQ2_0/PTQ1_0, whose weights are rotated;
for any unrotated GGUF the ANE path is live today.

**But it buys nothing yet**, and the log says why: the calibrated split puts
only **50% of the FFN on the ANE and reports 1.6% error from running that part
on the GPU alone** — i.e. the ANE's contribution is small and the split barely
changes the cost. Measured decode, three runs each, same build and prompt set:

| ANE | marginal decode |
|---|---|
| on | 37.80 / 34.63 / 34.42 tok/s |
| off | 35.30 / 37.12 tok/s |

Indistinguishable. Two reasons, both actionable:

1. **`share = 0.50` is a constant.** `AneFfnSetting::Given{share, minimumRows}`
   exists in `AneFfnStartup.hpp:19-27` but **nothing in the CLI sets it** — only
   the dev oracle test parses `--ane-ffn-share`. The production server cannot
   ask for a bigger ANE share, so it always calibrates to the default half.
2. **The split only covers prefill FFN.** `AneFfn` is driven from
   `Runtime::prefillAsync`; decode runs entirely on the GPU. Since the model is
   weight-bound in decode (7.93 GiB per cycle), the ANE is idle exactly where a
   throughput win would show.

**Action, highest value per day of work:** expose `--ane-share` /
`--ane-minimum-rows` in the real CLI, then sweep share 0.5 → 0.75 → 1.0 on the
ordinary GGUF and measure. If share 1.0 works, the prefill FFN leaves the GPU
entirely — on a 10 GB IQ3_XXS that is ~40% of the weights. This is a flag and a
sweep, not a kernel rewrite.

---

## 1. Context window

### What is true today

| model | KV | context | note |
|---|---|---|---|
| Bonsai PQ2_0/PTQ1_0 (27B ternary) | int4 | **262,144** (model max) | KV pool reports **360,448** tokens capacity |
| Bonsai, int8 | int8 | 163,833 (180,217 without ANE) | |
| Qwen3.8-27B IQ3_XXS (10 GB) | int8 | 16K in the test above; **57,337** at auto budget | weights alone are 10 GB |

So on the ternary model the context is already at the model's declared maximum,
and the *pool* has 37% headroom beyond it. Context is not limited by KV format
any more — it is limited by the model's declared `context_length`.

### The three real levers, in order

**(a) RoPE scaling — the only way past a model's declared maximum.**
`requireMetadata` rejects RoPE scaling outright (a planner test pins it:
"planner refuses RoPE scaling"). Nothing in the engine can extend a model beyond
its trained window. For a 24 GiB machine with a 262K-capable model this is the
single biggest *capability* gap: YaRN/NTK scaling is a `RoPE.cpp` + metadata
change plus a quality check, and it is how every other engine does it.

**(b) SSD tier is unreachable from the CLI — a wiring bug, not a design limit.**
`ExtendedKvTier` (hot RAM + cold SSD, `--hot-tier-tokens`,
`--max-context-extended`, `--prefetch-pages`) exists and is constructed in
`RuntimeResources.mm:720` behind `ssdKvEnabled && maxContextExtended > 0`.
But `NativeArguments::maxContextExtended` is **never copied** into
`RuntimeResourcesConfig` or `EngineConfig` — `main.mm` wires `ssdKvEnabled`,
`ssdKvPath` and `hotTierTokens` and skips it. So `maxContextExtended` stays 0 and
the cold tier is dead code on every run, even with `--ssd-kv on`.
The Python server does not expose `--ssd-kv` at all.
**Fix: two lines** (`config.resources.maxContextExtended = arguments.maxContextExtended;`
and the same into `EngineConfig`), plus surfacing the flag in
`serve_options.py`. Then 1 TB of SSD becomes a real context extension instead of
a dead branch — this is the cheapest large win available.

**(c) `--kv-swap-*` is gone and should stay gone.** It could not return a live
request's RAM to the pool (only `Cache::demoteKv` may); `ExtendedKvTier` is the
mechanism that actually works.

---

## 2. Tokens per second

### Measured reality

| what | number |
|---|---|
| decode, 1 request, ternary | 36–39 tok/s marginal |
| **aggregate at concurrency 4** | **90.3 tok/s** |
| aggregate at 8 / 16 | 92.0 / 91.7 |
| prefill 4.3K / 17.1K / 68.2K / 136.3K | 400 / 383 / 300 / 236 tok/s |
| prefill 179.9K | 201 tok/s |

Prefill fits `t(n) = 2.41147 ms·n + 13.3943 ns·n²` (all points within 1.7%).

### Where the headroom actually is

**(a) The weight path is compute-bound at 76% of peak — nearly done.**
`2 × 24.353 GFLOP / 2.411 ms = 20.2 TFLOP/s` against a 26.5 TFLOP/s peak, at
**1.31 GB/s of DRAM (0.4% busy)**. There is no bandwidth to reclaim; only ~12%
of the prefill time is left in that path.

**(b) Attention runs at 14.7 TFLOP/s — 55% of peak, the cheapest real target.**
The gap is not KV traffic: doubling KV bytes (int4→int8) changed prefill by
**−2.2%** instead of the predicted +23%, which refutes the bandwidth diagnosis
outright. Attention is *compute*-inefficient, the signature of a skinny 8-row
GEMM (`SPLASH_PREFILL_ATTENTION_TILE_ROWS = 8`) that under-fills the matrix
units. The softmax kernel maps one lane per 8 tokens and caps at 256 threads, so
12 rows already needs 288 and **silently drops rows** — that cap, not the split
count, is what has to change. `prefillSplits()` returning 1 for every chunk is
real but worth only 3–9% (measured), because 1,024 threadgroups already fill 16
cores; splits remove no FLOPs.

**(c) Batch width is an ABI wall at 4, and it is not worth breaking.**
`ExecutionLimits::maximumBatchWidth = 4` sizes the decode arenas, lane state and
draft persistent keys. Aggregate plateaus at 4 (90.3 → 92.0 → 91.7 across
4/8/16 concurrency). Widening to 16 means rebuilding arenas, KV paging and draft
buffers for **3.8%**. Skip it.

**(d) The prefix cache is the biggest practical win and already works: 66.6×.**
A 20.7K-token document: 56.8 s cold, **0.9 s on repeat**; multi-turn reports
`kv_hit_tokens: 103,680`. For RAG and agents this is the number that matters, and
it costs nothing — it just has to be used (stable prompt prefixes) and it should
be documented and exposed as a metric.

---

## 3. Metal GPU utilisation

- Decode: GPU is **99.4% busy**, prefill 99.7%. There is no idle GPU to reclaim;
  the work is simply not using the matrix units efficiently in attention (55%).
- The single highest-value Metal change is therefore **the attention tile
  height**, and it is blocked by a kernel structural limit (256-thread cap in
  `splash_attention_page_softmax`), not by a constant. Doing it means: raise the
  threadgroup budget or split the row loop across simdgroups, then prove
  correctness against the scalar oracle at 131K/180K/260K (the harness for that
  already exists from the int4 audit).
- `FmtPTQ10`/`FmtPQ20` share plane geometry, so there is nothing to win by
  swapping them — proven, do not revisit.

---

## 4. SSD utilisation (1 TB, currently almost unused)

Today: a 32 GiB **temporary** cache in `$TMPDIR` for KV pages and states, plus
persistent cache support. Measured: `SSD cache: up to 32.0 GiB ... KV pages of
1040 KiB`.

What is missing to actually use 1 TB:

1. **Wire `maxContextExtended`** (§1b) — turns `ExtendedKvTier` on, giving a hot
   RAM tier plus a cold SSD tier with `--prefetch-pages` read-ahead. This is the
   designed mechanism for context beyond RAM and it is one line from working.
2. **Point the cache at a real directory on the 1 TB volume** rather than
   `$TMPDIR` (which macOS may purge): `--cache-dir` / `--ssd-kv-path`.
3. **Persistent prefix cache across restarts** — `--persistent-cache` exists;
   combined with the 66.6× prefix hit rate it means a document's KV survives a
   server restart. Worth verifying end to end, since it is the difference between
   "warm after first query" and "warm immediately".

---

## 5. Concrete plan, ordered by value per effort

| # | Change | Effort | Expected | Risk |
|---|---|---|---|---|
| 1 | Wire `maxContextExtended` + surface `--ssd-kv*` in the Python server | hours | SSD context tier becomes real; 1 TB usable | low |
| 2 | Expose `--ane-share` / `--ane-minimum-rows`, sweep on the ordinary GGUF | 1 day | ANE actually used; up to ~40% of prefill FFN off the GPU | low |
| 3 | Attention tile height (256-thread cap) | days | attention 55% → toward 76%; ~10–20% of long prefill | medium |
| 4 | RoPE scaling (YaRN/NTK) | days | context past the declared max — the only path to it | medium, quality risk |
| 5 | Persistent prefix cache end to end | 1 day | instant warm start on repeated documents | low |
| 6 | Batch width > 4 | weeks | +3.8% | not worth it |

## 6. Dead weight still in the tree (should go)

`--ane-layers` is parsed and reaches `HybridQuantConfig`, but per-layer format
selection was never implemented (the GGUF decides), and the ANE split declines
rotated layers anyway — so on Prism models it is decorative. `AneFfn::unsupported`
carries a comment saying plane-view validation is "deferred"; it is not deferred,
it is skipped, and the real check happens later in `takesPlaneViews`.

Verified removed this session: 11 flags that did nothing, 4 unreachable classes.

---

## 7. What I would do first, and why

**Wire the SSD tier (1), then sweep the ANE share (2).** Both are wiring and
flags, both are measurable within a day, and together they are the only two
changes that touch the two resources that are currently idle — 1 TB of SSD and
the Neural Engine. Everything else either needs a kernel rewrite (attention
tiles, RoPE) or is not worth the rebuild (batch width).

The engine's GPU and DRAM are already near saturation on the paths that work;
the remaining wins are in the hardware that is switched off.
