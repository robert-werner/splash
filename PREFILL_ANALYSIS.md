# Prefill: row budget and attention history splits

Two levers on prefill time, measured on this machine (24 GiB, Apple10, int4 KV)
against `t(n) = a*n + b*n^2` with **a = 2.41 ms/token** and
**b = 13.39 ns/token^2** (four prompt lengths within 1.7%, the 179.9K holdout
within 3%). Note the units: `a` is milliseconds per token, which is what makes
`1/a = 415 tok/s` the asymptote and `a/b = 180,036` the break-even.

The split is 50/50 at 180K tokens and 59/41 attention at 262,144, and the
break-even of the two terms is 180,036 tokens. **The row budget only moves the
linear half** — it cannot take prefill past the pure-linear asymptote of
415 tok/s — so the verdict on it is "measurable but capped", and the quadratic
half is where the time is.

---

## 1. Raising `SPLASH_PREFILL_TOKEN_BUDGET` to 4096 / 8192

### 1.1 The memory model

`model::prefillTensorBytes` (runtime/model/RuntimeArenas.cpp:8) sizes every
tensor of the shared prefill arena from `kPrefillRows`
(= `ExecutionLimits::prefillTokenBudget`), and `plannedPrefillBytes` sums them
after `alignUp` to a 16 KiB host page each (runtime/Checked.hpp:52). Sizes
below are for the shipped Qwen3.8-27B geometry — hidden 5120, 5 capture layers,
packed GDN width 16640, dense FFN 17408, attention 24 query heads / 4 KV heads
/ 256, draft width 5120 — and reproduce the boot plan's measured
`shared_prefill_bytes` exactly: **834,879,488 B at 2048**.

| Tensor | scales with | 2048 B | Δ 2048→4096 B | Δ 2048→8192 B | note |
|---|---|---:|---:|---:|---|
| `Hidden0` | row | 20,971,520 | 20,971,520 | 62,914,560 | hidden, bf16 |
| `Hidden1` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `InputTokens` | row | 8,192 | 8,192 | 24,576 |  |
| `Normalized` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `Captured` | row | 104,857,600 | 104,857,600 | 314,572,800 | 5 capture layers x 5120 |
| `GdnPacked` | row | 68,157,440 | 68,157,440 | 204,472,320 | packed GDN input |
| `GdnQueries` | row | 8,388,608 | 8,388,608 | 25,165,824 |  |
| `GdnKeys` | row | 8,388,608 | 8,388,608 | 25,165,824 |  |
| `GdnValues` | row | 25,165,824 | 25,165,824 | 75,497,472 |  |
| `GdnDecay` | row | 393,216 | 393,216 | 1,179,648 | fp32 |
| `GdnBeta` | row | 196,608 | 196,608 | 589,824 |  |
| `Recurrent` | row | 25,165,824 | 25,165,824 | 75,497,472 |  |
| `GdnHidden` | row | 25,165,824 | 25,165,824 | 75,497,472 |  |
| `GdnOutput` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `GateIntermediate` | row | 71,303,168 | 71,303,168 | 213,909,504 |  |
| `Intermediate` | row | 71,303,168 | 71,303,168 | 213,909,504 |  |
| `FullPacked` | row | 58,720,256 | 58,720,256 | 176,160,768 |  |
| `FullQueries` | ragged | 26,689,536 | 25,165,824 | 75,497,472 |  |
| `FullAttention` | ragged | 26,689,536 | 25,165,824 | 75,497,472 |  |
| `AttentionPartials` | tile | 50,331,648 | 50,331,648 | 150,994,944 | per 8-row tile: 8*24*256*4 B |
| `AttentionStatistics` | tile | 393,216 | 393,216 | 1,179,648 |  |
| `AttentionHidden` | row | 25,165,824 | 25,165,824 | 75,497,472 |  |
| `AttentionOutput` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `TargetPositions` | row | 24,576 | 24,576 | 73,728 | M-RoPE, 3 axes |
| `DraftPositions` | row | 8,192 | 8,192 | 24,576 |  |
| `TargetInverseFrequencies` | fixed | 128 | 0 | 0 |  |
| `DraftInverseFrequencies` | fixed | 256 | 0 | 0 |  |
| `RopeCos` | row | 262,144 | 262,144 | 786,432 | fp32 |
| `RopeSin` | row | 262,144 | 262,144 | 786,432 | fp32 |
| `ContextProjected` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `ContextHidden` | row | 20,971,520 | 20,971,520 | 62,914,560 |  |
| `ContextKv` | row | 8,388,608 | 8,388,608 | 25,165,824 |  |
| `DraftRopeCos` | row | 524,288 | 524,288 | 1,572,864 | fp32 |
| `DraftRopeSin` | row | 524,288 | 524,288 | 1,572,864 | fp32 |
| `ChunkKeys` | ragged | 4,448,256 | 4,194,304 | 12,582,912 |  |
| `ChunkValues` | ragged | 4,448,256 | 4,194,304 | 12,582,912 |  |
| `LinearPartials` | fixed | 1,310,720 | 0 | 0 | 32-row split scratch |
| `LinearCounters` | fixed | 320 | 0 | 0 |  |
| `LinearRotated` | row | 71,303,168 | 71,303,168 | 213,909,504 | rotated input, k = 17408 |

**Total: 834,879,488 B (796.20 MiB) at 2048 → 1,664,778,240 B (1587.66 MiB)
at 4096 → 3,324,624,896 B (3170.61 MiB) at 8192.**

| budget | prefill arena | delta vs 2048 | bytes per row |
|---:|---:|---:|---:|
| 2048 | 796.20 MiB | — | — |
| 4096 | 1587.66 MiB | **+791.45 MiB** | 405,224 B/row |
| 8192 | 3170.61 MiB | **+2374.41 MiB** | 405,224 B/row |

So the cost is linear and steep: **+791 MiB for 4096, +2374 MiB for 8192**,
about 0.77 MiB per extra chunk row. Four tensors carry 40% of it
(`Captured` 51,200 B/row, `GateIntermediate` + `Intermediate` + `LinearRotated`
34,816 B/row each).

Two rows of the table are worth noting because they are *not* purely linear:
`FullQueries`/`FullAttention`/`ChunkKeys`/`ChunkValues` are sized at
`kRaggedAttentionRows = kPrefillRows + kLaneCount * (kPageTokens - 1)` (2048 +
4*31), so they grow by 12,288 B/row instead of 12,288 and 2,048 B/row — the
fixed 124-row ragged tail is amortised differently, which is why 4096 rounds
those four *down* to 16 KiB.

### 1.2 What it costs in context

`EngineMemoryPlan::evaluateEngineMemoryPlan` (runtime/engine/MemoryPlan.cpp:264)
puts the whole prefill arena in `fixedRuntimeBytes`, then sizes the KV pool from
what is left. Every MiB the arena takes comes straight out of the pool. Using
the boot plan's own numbers for this machine — target 6,915.29 MiB, draft
1,207.97 MiB, vision 887.16 MiB, decode 269.89 MiB, reserves 768 MiB, state
staging 152.62 MiB, lane state 339.62 MiB, hard budget 17,162.25 MiB — at int4
KV (1,064,960 B per 32-token model page, 16 attention layers):

| budget | fixed runtime | elastic state/KV | KV capacity | usable context | pool slack past 262,144 |
|---:|---:|---:|---:|---:|---:|
| 2048 | 10.74 GiB | 6.02 GiB | 360,448 tok | 262,144 | +1.55 GiB |
| 4096 | 11.51 GiB | 5.25 GiB | 311,296 tok | **262,144** | **+0.77 GiB** |
| 8192 | 13.06 GiB | 3.70 GiB | 212,992 tok | **212,985** | −0.77 GiB |

- **4096 fits without shrinking context, but with 0.77 GiB of slack, not the
  ~1 GiB the task asked for.** The KV extent quantises in 256-page (8,192
  token) units, so capacity lands at 311,296 tokens and the tail after 262,144
  is only ~50K tokens of pool that the extent arithmetic can actually use.
- **8192 does not fit.** It leaves 212,992 tokens of KV, i.e. it would cut the
  usable context from 262,144 to 212,985 (−19%). Rejected on that alone.
- This is all int4. At int8 KV the pool is half the size and even 2048 only
  reaches 180,217 tokens on this machine; the deltas above are identical
  (the arena does not depend on KV format), so 4096 would cost int8 users
  25K tokens of context. That asymmetry is the strongest argument for not
  changing the default.

### 1.3 Everything that would have to change, and what actually breaks

| Site | What it is | 4096 | 8192 |
|---|---|---|---|
| `metal/abi/ExecutionGeometry.h:14` | the constant | fine | fine |
| `ops/Linear.cpp:23` | `w.rows > budget` workload check | fine | fine |
| `ops/Linear.cpp:192` | `static_assert(budget % GGUF_PREFILL_ROWS == 0)` (128) | fine | fine |
| `ops/Linear.cpp:193` | rotated scratch bound, 34,816 B/row | fine | fine |
| `ops/MoE.cpp:217` | prefill rows check | fine | fine |
| `ops/AneFfn.hpp:47` | `kMaximumRows`, plus `static_assert((max-512) % 128 == 0)` | fine (29 functions) | fine (61) |
| `ops/PagedAttention.hpp:65,66` | `kChunkedPrefillMaximumRows`, tile rows | fine | fine |
| `metal/.../paged_attention_tile.h:30,32` | kernel contract `rows/stride <= budget` | fine | fine |
| `metal/.../paged_store_row.h:10` | `SplashChunkMaximumRows` | fine | fine |
| `engine/Status.cpp:374` | `"prefill_" + budget` warmup step name | renames the step | renames it |
| `model/Model.hpp:446` | duplicate literal 2048 (now derived) | **was a silent mismatch** | same |
| `dev/tests/engine/model_loading_test.cpp:113` | `== 2048` pin | **was a hard failure** | same |

Nothing else in the kernels depends on the budget: the GGUF prefill tile is a
fixed `GGUF_PREFILL_ROWS = 128` rows over a grid of `ceil(rows/128)` groups
(`shared/gguf_linear.metal:253`), the attention tile is a fixed 8 rows, the GDN
scan stages a fixed 16-token block, and the store kernel dispatches
`2 * chunk_tokens * kvHeads` threadgroups. **No threadgroup memory, register
count or ABI struct size depends on the row budget** — the structs
(`SplashChunkedPrefillParams` 24 B, `SplashPrefillAttentionParams` 28 B,
`GgufPrefillParams` 16 B) carry counts, not sizes. So the kernels are safe at
4096 and 8192; the budget is purely a memory trade.

### 1.4 What was implemented

- `SPLASH_PREFILL_TOKEN_BUDGET` is now overridable at compile time
  (`#ifndef` guard) and documented with the numbers above. **2048 stays the
  default**, because 4096 buys 0.77 GiB of headroom instead of the ~1 GiB
  required and because it costs int8-KV users 25K tokens of context.
- `ExecutionLimits::prefillTokenBudget` (model/Model.hpp:446) was a second,
  hand-written `2048`; it now derives from the ABI constant, which removes a
  real drift hazard: raising the ABI constant alone would have left the
  scheduler, arenas and warmup at 2048 while the kernels validated against
  4096, and `RuntimeResources.mm:33`'s `static_assert` would have fired.
- `model_loading_test.cpp` pinned `== 2048`; it now checks the value against
  the ABI constant and that it is a multiple of 128 (the prefill tile), so it
  is correct at any legal budget instead of only at 2048.
- Every other static assertion (`Linear.cpp:192`, `AneFfn.hpp:48`) already
  holds at any multiple of 128, so no assertion was weakened or removed.

**Verified by building, not only by arithmetic.** `SPLASH_PREFILL_TOKEN_BUDGET`
was overridden on the compiler command line for the whole engine (C++, ObjC++
and Metal) and the arena measured through `model::plannedPrefillBytes` on the
shipped 27B geometry:

| budget | `plannedPrefillBytes` | MiB | delta |
|---:|---:|---:|---:|
| 2048 | 834,879,488 | 796.20 | — |
| 4096 | 1,664,778,240 | 1587.66 | **+791.45 MiB** |
| 8192 | 3,324,624,896 | 3170.61 | **+2374.41 MiB** |

2048 matches the boot plan's `shared_prefill_bytes` byte for byte, and 4096
matches the table above exactly, so the tensor model and the production sizing
code agree. Both alternate budgets also build clean: `make` with the define
applied to `ENGINE_CXXFLAGS`, `ENGINE_OBJCXXFLAGS` and `PROD_METALFLAGS`
compiles the whole engine and metallib with no warnings under
`-Wall -Wextra -Werror`, which is the evidence that no kernel, threadgroup
allocation or static assertion breaks at 4096 or 8192.

The context figures are arithmetic from `evaluateEngineMemoryPlan` and
`Layout::extentPagesFor` against this machine's boot plan; they were not
re-measured at 4096 because that would mean loading the model, which the
parent's measurement run ruled out.

---

## 2. The quadratic term: `prefillSplits` never splits

### 2.1 The defect

`ops/PagedAttention.cpp:186`:

```cpp
uint32_t prefillSplits(uint32_t tiles) {
  constexpr uint32_t maximum = SPLASH_PREFILL_ATTENTION_MAXIMUM_SPLITS;  // 32
  return std::clamp(maximum / tiles, 1U, maximum);
}
```

`tiles = ceil(rows / SPLASH_PREFILL_ATTENTION_TILE_ROWS)` with tile rows = 8,
so a 2048-row chunk has 256 tiles and `32 / 256 == 0` clamps to **1**. The
split dispatch is `{kvHeads, tiles, splits}` = `{4, 256, 1}`: each query
tile's threadgroup walks the whole committed prefix by itself, and attention
per chunk is linear in history length. This is the n² term.

The formula is only self-consistent for small chunks, where it degenerates the
other way: 8 rows gives 1 tile and 32 splits, i.e. 32 threadgroups all reading
one page. It was written as `32 / tiles` against an 8-row tile and never
revisited; `git log -S` puts it in the initial import, and
`paged_attention_plan_test.mm:99` re-derives the same expression, so the test
asserts the formula rather than the intent.

Measured through the production code (a probe linked against
`build/engine/libsplash.a`, calling `PagedAttention::prefillPlan`):

| rows | tiles | splits | split grid | partials | statistics |
|---:|---:|---:|---|---:|---:|
| 8 | 1 | 32 | 4 x 1 x 32 | 6.00 MiB | 48.0 KiB |
| 64 | 8 | 4 | 4 x 8 x 4 | 6.00 MiB | 48.0 KiB |
| 128 | 16 | 2 | 4 x 16 x 2 | 6.00 MiB | 48.0 KiB |
| 136 | 17 | 1 | 4 x 17 x 1 | 3.19 MiB | 25.5 KiB |
| 512 | 64 | 1 | 4 x 64 x 1 | 12.00 MiB | 96.0 KiB |
| 1024 | 128 | 1 | 4 x 128 x 1 | 24.00 MiB | 192.0 KiB |
| **2048** | **256** | **1** | **4 x 256 x 1** | **48.00 MiB** | **384.0 KiB** |

### 2.2 What tile rows would give splits > 1

Splits only become > 1 when `tiles < 32`, i.e. **tile rows ≥ 128** at a
2048-row chunk:

| tile rows | tiles at 2048 rows | splits | workspace slots (tiles*splits) |
|---:|---:|---:|---:|
| 8 | 256 | 1 | 256 |
| 16 | 128 | 1 | 128 |
| 32 | 64 | 1 | 64 |
| 64 | 32 | 1 | 32 |
| **128** | 16 | **2** | 32 |
| **256** | 8 | **4** | 32 |

Note what this means: raising tile rows does not *add* work, it *re-partitions*
the same work — the workspace is `tiles * splits` slots and the product is
pinned near 32 by the formula. So at 128 rows the workspace is 2x the 8-row
plan's **per 2048-row chunk**, and 4x at 256 rows — but the 8-row plan at 2048
rows is already 48 MiB, because it has 256 tiles x 1 split.

Concretely, `attentionWorkspace(rows, headDim)` = `{rows * 256 * 4 B,
rows * 2 * 4 B}`, so:

| plan | slots | partials | statistics |
|---|---:|---:|---:|
| 2048 rows, tile 8, splits 1 (today) | 256 | 48.00 MiB | 384 KiB |
| 2048 rows, tile 128, splits 2 | 32 | 6.00 MiB | 48 KiB |
| 2048 rows, tile 256, splits 4 | 32 | 6.00 MiB | 48 KiB |
| 2048 rows, tile 128, splits 4 (explicit) | 64 | 12.00 MiB | 96 KiB |
| 2048 rows, tile 128, splits 8 (explicit) | 128 | 24.00 MiB | 192 KiB |

**So the "workspace grows 2x at 128, 4x at 256" intuition does not hold against
the real code: it shrinks 8x, because the arena is currently sized for 256
one-split tiles.** `prefillWorkspace` takes the max over every `tiles` from 1 to
`ceil(maxRows/tileRows)` of `tiles * splits`, which at 8-row tiles is 256 slots
and at 128-row tiles is 32 — the existing 48 MiB bound already covers tile-128
plans with room to spare. That is good news: history parallelism here is
**memory-free**, and 48 MiB of arena is already paid for.

### 2.3 Do the kernels support splits > 1?

Yes, and the code is careful about it — this is not a broken path, it is an
unused one.

- **Split** (`prefill_attention_split_phase`, kernels/prefill/paged_attention.metal:8):
  reads `split = group.z`, computes `page_begin = split * per_split` and
  `page_end = min(pages, page_begin + per_split)` with
  `per_split = ceil(pages / splits)`, writes
  `slot = (tile * KVHeads + kv_head) * splits + split`. Each split writes only
  its own pages' pages of history; a split with no pages returns immediately
  (`if (page_begin >= pages) return;`). Per-row causal masking is applied
  inside the split (`splash_attention_page_softmax` clamps to
  `committed + min(query_row, active_rows-1) + 1`), so a split covering pages
  past a row's causal limit contributes only masked (zero) probabilities and
  `-INFINITY` to that row's running max. **Correct for any splits.**
- **Reduce** (`splash_attention_reduce_row`, kernels/common/paged_attention_tile.h:336):
  recomputes `per_split` and `written = ceil(pages / per_split)` — i.e. it
  derives exactly how many splits wrote data, ignores the unwritten tail, takes
  `row_maximum` over the written splits, weights each by
  `exp(max_i - row_max)`, and accumulates numerator/denominator **in split
  order, serially**, with a comment explaining that a parallel sum of the
  denominator changes speculative acceptance on real prompts. That is a
  numerically stable online-softmax merge, correctly ordered.
- The slot layout is `[tile][KV head][split]` in both kernels and in the
  arena's `partials`/`statistics`, and
  `paged_attention_plan_test.mm:39` already enumerates grids with
  `splits > 1` and asserts the slot bijection — the *plan* side is tested at
  splits 2 and 4 (it requires `multipleSplits`). What is **not** tested is a
  GPU run with `split_count > 1`: `q8_chunked_prefill_metal_test.mm` builds its
  params from `plan.splits`, and every chunk it exercises (32, 77, 96, 1056
  rows) lands on splits = 1 except the 8-row case. So the split/reduce *kernels*
  run at splits = 1 in every end-to-end test, and the multi-split path is
  covered only by the plan test's bookkeeping.

**Verdict: the path looks correct by inspection and is exercised at splits > 1
only in the plan test. It is not a known-broken path, but it is unproven on the
GPU, and that must be fixed with a test before any behaviour change lands.**

### 2.4 The hard blocker: threadgroup memory and the softmax tile

Tile rows ≥ 128 is not a one-line change. The split kernel allocates, per
threadgroup, `M = tileRows * queryHeadsPerKVHead` fused rows:

```
threadgroup float  scores[M * 32];        // 4 B each
threadgroup bfloat probabilities[M * 32]; // 2 B each
threadgroup float  row_max[M], row_sum[M], previous_scale[M];  // 12 B each
```

| tile rows | M (kv4_g6, x6) | threadgroup B | vs 32 KiB | softmax lanes needed (4*M) | vs 256 threads |
|---:|---:|---:|---|---:|---|
| 8 | 48 | 9,792 | ok | 192 | ok |
| 16 | 96 | 19,584 | ok | 384 | **over** |
| 24 | 144 | 29,376 | ok | 576 | **over** |
| 32 | 192 | 39,168 | **over** | 768 | **over** |

`SplashPrefillTileRows = 16` already needs 384 threads for the page softmax
(`static_assert(LanesPerRow * FusedRows <= 256)` in
`paged_attention_tile.h:96`) and 32 rows needs 39 KiB of threadgroup memory,
past this device's 32 KiB. **So tile rows cannot simply be raised to 128 or
256: the softmax's one-lane-per-four-tokens-per-row structure and the
threadgroup budget both cap the tile at 8–12 rows, and 12 rows still gives
splits = 1 at 2048 rows (`tiles = 171`).**

The fix is therefore *not* "raise tile rows". It is to **decouple the history
split count from the query tile count**: size `splits` from the history
(`ceil(pages / pagesPerSplit)` or a fixed 4–8), not from `32 / tiles`. That
leaves the tile at 8 rows (threadgroup memory unchanged, softmax unchanged),
multiplies only the number of threadgroups, and needs the workspace to hold
`tiles * splits` slots — which at 2048 rows x 4 splits is 1024 slots = 192 MiB
of partials, and at 8 splits 384 MiB. That is the real memory question.

### 2.5 Does it fit?

Against the boot plan (796.20 MiB prefill arena, deficit 0, 1.55 GiB of KV
slack at int4 and 262,144 context):

| splits at 2048 rows, tile 8 | partials | statistics | arena | delta | fits |
|---:|---:|---:|---:|---:|---|
| 1 (today) | 48.00 MiB | 384 KiB | 796.20 MiB | — | yes |
| 2 | 96.00 MiB | 768 KiB | 844.20 MiB | +48 MiB | yes |
| 4 | 192.00 MiB | 1.5 MiB | 940.20 MiB | +144 MiB | yes (KV slack 1.41 GiB) |
| 8 | 384.00 MiB | 3.0 MiB | 1132.20 MiB | +336 MiB | yes (KV slack 1.22 GiB) |
| 16 | 768.00 MiB | 6.0 MiB | 1516.20 MiB | +720 MiB | yes, but 0.85 GiB slack |
| 32 | 1536.00 MiB | 12 MiB | 2284.20 MiB | +1488 MiB | **no**, KV pool loses 1.45 GiB |

So splits 2–8 are affordable *and* keep ≥ 1.2 GiB of KV headroom at 262,144
context, at int4. At int8 KV the same deltas apply and the pool has no slack to
give — 4 splits would cost int8 users ~10K tokens of context, 8 splits ~21K.

### 2.6 Verdict

- **Raising `SPLASH_PREFILL_ATTENTION_TILE_ROWS` is the wrong lever and is
  capped at 8–12 rows by the softmax's 256-thread structure and by 32 KiB of
  threadgroup memory.** At 12 rows, `32/tiles` still returns 1 for a 2048-row
  chunk, so the formula would still not split.
- **The defect is real**: `prefillSplits` computes splits from the query tile
  count, and at any realistic chunk the clamp pins it to 1, so no threadgroup
  parallelises history and attention per chunk is linear in n. The fix is to
  size splits from the history, not the tile count.
- **The split and reduce kernels do support splits > 1** — split ids index
  partials correctly, the reduce recomputes which splits wrote and merges them
  in split order with an online-softmax rescale. Nothing in that path looked
  wrong on reading.
- **But it is unproven on the GPU**: every end-to-end Metal test lands on
  splits = 1. Multi-split attention must get a GPU test before the plan changes
  behaviour — a chunk with a long committed prefix and `split_count` 2, 4 and 8,
  compared against the scalar reference the existing test already has, plus the
  `split_count = 0` / `33` contract rejections it already checks.
- **It is affordable**: 4 splits costs +144 MiB of prefill arena and leaves
  1.41 GiB of KV headroom at 262,144 context (int4). 8 splits costs +336 MiB
  and leaves 1.22 GiB. Both fit; neither touches threadgroup memory or any ABI.
- **Expected gain**: history parallelism turns the per-chunk attention walk from
  `n` serial pages into `n/splits`, so the 13.39 ns/token² term should fall by
  close to a factor of `min(splits, occupancy headroom)`. With 4 KV heads x 256
  tiles there are already 1024 threadgroups at splits = 1 — enough to fill a
  16-core GPU several times over, which is why the win will be *far* below 4x:
  the GPU is not idle, it is running many tiles that each re-read the whole
  prefix. What splits change is the **total page traffic and matmul work per
  chunk is unchanged** — splits do not reduce FLOPs, they only redistribute
  them across more threadgroups. **So splits help only if the current dispatch
  is latency- or occupancy-bound, not if it is already saturating the GPU.**

  That distinction matters and I cannot settle it by arithmetic. The fitted
  quadratic term implies attention runs at **14.7 TFLOP/s** (2*D*QH*L*n² =
  2*256*24*16*n² over 13.39 ns*n² seconds — the ratio is constant in n, which
  is itself evidence the term really is attention FLOPs and not a fixed
  per-chunk cost). Whether 14.7 TFLOP/s is "saturated" depends on the GPU's
  fp32 rate, which I do not have a trustworthy figure for here.

  But there is a stronger structural argument that **splits alone will not fix
  this**. With 4 KV heads x 256 query tiles the dispatch already has 1,024
  threadgroups at splits = 1 — several waves on a 16-core GPU. Splitting
  history does not remove a single FLOP: every split still multiplies the same
  Q tile by the same K pages. It only shortens each threadgroup's serial page
  walk, which helps if the kernel is latency-bound on that walk and buys
  nothing if it is throughput-bound on the matmuls. Since the GPU already has
  more threadgroups than it can run at once, **the likely binding constraint is
  matmul throughput, not parallelism**, and the n² term is then a real
  14.7 TFLOP/s wall rather than a scheduling artefact.

  **So I measured it instead of arguing.** A probe linked against the
  production library encodes the real `prefill_attention_q8_split` and
  `prefill_attention_reduce` pipelines over the `AttentionFixture` KV pool, and
  varies only `split_count` (1, 2, 4, 8), taking the median GPU time of 3-5
  submissions. 2048-row chunk, int8 KV, one attention layer, GPU validation
  off:

  | committed history | splits 1 | 2 | 4 | 8 |
  |---:|---:|---:|---:|---:|
  | 30,000 | 106.27 ms | 1.039x | 1.053x | **1.064x** |
  | 120,000 | 431.41 ms | 1.027x | 1.053x | **1.085x** |

  **Splits buy 3-9%, not 2-4x.** The scaling with history is telling: at 120K
  the relative gain is slightly *larger* than at 30K, i.e. the split helps the
  page walk a little and the walk is a small share of the time. The kernel is
  **matmul-throughput-bound**, and the n² term is a genuine ~14.7 TFLOP/s wall,
  not a scheduling artefact. Splitting history does not remove one FLOP, and
  with 1,024 threadgroups already in flight at splits = 1 the GPU has all the
  parallelism it can use.

  **So the `prefillSplits` defect is real but is not the cause of the n² term,
  and fixing it is not worth the risk.** The honest lever is fewer FLOPs per
  token-pair: a shorter KV head dimension, a quantised or lower-precision QK,
  or moving layers off full attention (sliding window / GDN, which the model
  already uses for 48 of its 64 layers).

**Plain verdict: leave the split count alone.** `prefillSplits` really does
return 1 for every realistic chunk and that is a defect in intent, but the
measured payoff of fixing it is 3-9%, so it is not the lever for the n² term
and is not worth changing kernel dispatch behaviour for. If it is ever fixed
anyway (it is cheap and the memory is already there), do it with a GPU test
first: multi-split attention is covered only by the plan test today. The
n² term needs fewer FLOPs, not more threadgroups.

---

## 3. Reproducing the numbers

- Prefill arena: `model::prefillTensorBytes` / `plannedPrefillBytes`
  (runtime/model/RuntimeArenas.cpp:8-141). The table's arithmetic reproduces
  the boot plan's 834,879,488 B exactly at 2048.
- Context: `evaluateEngineMemoryPlan` (runtime/engine/MemoryPlan.cpp:264) and
  `Layout::extentPagesFor` (runtime/ops/PagedKv.hpp:214). Inputs are the boot
  plan's own figures from a 24 GiB Apple10 machine at int4 KV.
- Splits and workspace: `PagedAttention::prefillPlan` / `prefillWorkspace`
  (runtime/ops/PagedAttention.cpp:192-256), read back through the production
  library rather than re-derived.
- The split measurement encodes `prefill_attention_q8_split` and
  `prefill_attention_reduce` from `build/splash.metallib` over
  `dev/tests/engine/AttentionFixture.hpp`'s KV pool, with the production
  pipelines' names and grids, varying only `split_count`. It is a timing
  probe, not a correctness oracle; `build/engine-tests/q8-chunked-prefill`
  passes under `MTL_SHADER_VALIDATION=1` at the shipped plan.
