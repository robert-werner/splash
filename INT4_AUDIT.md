# INT4 KV format — end-to-end audit

Branch `int4-audit`, from `c8b5d3dc`. Scope: is `--kv-format int4` correct enough to
serve a ~180K-token prompt? Every claim below is tied to a file and line, a
compile-time assertion, or a test that was actually run.

**Verdict: the int4 KV path is complete and numerically correct, and the 180K run is
safe.** Eight int4 kernels exist and are in the shipped metallib; the four dispatch
sites select them; the host byte arithmetic agrees with the shader ABI; the nibble
ownership rule holds by construction (proof below); and the int4 attention path was
replayed on the GPU against a scalar oracle at 131,072 / 180,224 / 260,096 tokens of
history, for both 24-query-head/4-KV-head and 16-query-head/2-KV-head geometries.
Two reporting defects were found and fixed (status JSON and the storage-format label
both described int4 values as dimension-major); one piece of dead code with a
*contradicting* layout is documented below and left in place.

---

## 1. The four kernel paths

Every KV format needs four paths. A missing one shows up as
`target prefill row is not finite` at bootstrap, because attention then reads pages
no store ever wrote.

| path | 4 KV heads | 2 KV heads | definition | selected by |
|---|---|---|---|---|
| prefill split | `prefill_attention_q4_split` | `prefill_attention_q4_split_kv2_g8` | `prefill/paged_attention.metal:93-94` | `PagedAttention.cpp` `prefillSplitPipeline` |
| prefill store | `prefill_attention_q4_store` | `prefill_attention_q4_store_kv2_g8` | `prefill/paged_attention_store.metal:47-48` | `prefillStorePipeline` |
| verify split | `verify_attention_q4_split` | `verify_attention_q4_split_kv2_g8` | `decode/paged_attention.metal:105-106` | `verifySplitPipeline` |
| verify store | `verify_attention_q4_store` | `verify_attention_q4_store_kv2_g8` | `decode/paged_attention_store.metal:62-63` | `verifyStorePipeline` |

All eight are present in the built metallib (verified with
`strings build/splash.metallib | grep -E '^(prefill|verify)_attention_q4'` → 8 names).
Each is instantiated from the same template as its INT8/BF16 sibling with
`CacheElement = int4b_format`, so the difference is exactly the format branch:

* **prefill split / verify split** — `common/paged_attention_tile.h`. `Quantized`
  includes `int4b_format` (line 200-201), so int4 takes the scaled softmax path that
  multiplies scores by the fp32 key scales and probabilities by the value scales.
  `TokenMajorValues` is true for int4 (line 225): the PV operand is built `{D, N}`
  with strides `{1, D}` and the descriptor passes `transpose_right = false`
  (`!TokenMajorValues`, line 249), i.e. the PV product skips the transpose for int4
  only. The QK operand is `{D, N}` for every format.
* **prefill store / verify store** — `common/paged_store_row.h`. The `int4b_format`
  branch (lines 76-117) quantizes a row to `[-7, 7]` about `simd_max(abs(value))`,
  writes the scale once per (head, token), and writes one byte per *pair* of
  dimensions from the even lane.
* **page addressing** — `common/kv_paging.h`. `splash_kv_value_index_of` (line 41-47)
  sends int4 values through `splash_kv_key_index` (token-major); every other format
  uses `splash_kv_value_element` (dimension-major). `common/kv_extent.h`'s
  `SplashKvAddressing::page` (lines 102-128) divides the element index by two for
  int4 to reach the byte, and `SplashKvPageBytes` computes `Data` from
  `SplashKvElementBits<int4b_format> = 4` and keeps `Scale` at INT8's
  `KVHeads * 32 * sizeof(float)`.
* **quantized gather / PV** — there is no separate gather kernel in the live path.
  The gather is the `splash_kv_tensor` operand construction in `kv_extent.h:69-82`
  plus the addressing above; the PV product is the `matmul2d` call in
  `paged_attention_tile.h:314`. Both are covered by the GPU test in §5.

`PagedAttention.cpp` has four `switch (format)` statements, one per path, each with an
explicit `case kv::Format::Int4` naming a `q4` kernel. There is no `default:` fallback
to int8 — the `BFloat16` case `break`s into the bf16 return, so a new enumerator would
be a `-Wswitch` error rather than a silent int8 fallback. `-Werror` is on.

## 2. Host byte math: Layout → PageStorage → KvPool → tier

| quantity | int8 | int4 | check |
|---|---|---|---|
| data bytes / layer / page | 32,768 | 16,384 | `PagedKv.hpp:147-157` halves for `Int4` |
| scale bytes / layer / page | 512 | 512 | `scaleBytesPerLayerPage` is format-independent except BF16 |
| bytes / layer / page | 66,560 | 33,792 | `2 * (data + scale)` |
| bytes / model page (16 layers) | 1,064,960 | 540,672 | `bytesPerModelPage` |
| bytes / token | 33,280 | 16,896 | page / 32 |
| extent alignment | 128 pages | 128 pages | `lcm(64KiB/data, 64KiB/scale)` |
| extent bytes | 130 MiB | 66 MiB | |

* `PageStorage::bytesPerPage()` returns `layout_.bytesPerModelPage()` and
  `spans()` slices each tensor with `splash_kv_page_bytes`/`splash_kv_offset` using
  the layout's `data` and `scale`, so host-visible page spans and shader-side
  region offsets are derived from the same two numbers.
* `KvPool::bytesPerPage()` forwards the storage's, and its accounting
  (`allocatedBytes`, `reclaimableBytes`) is `extents * extentPages * bytesPerPage`.
* The disk tier's slot is `SlotFile::slotBytesFor(pool.bytesPerPage())` — 540,672 B
  is already a whole number of 16 KiB host pages (33), so `SlotFile`'s
  `slotBytes % kHostPageBytes` guard passes without rounding. A page therefore
  occupies exactly one slot, as at int8.
* The persistent-cache namespace string includes `kv::formatName(format)`
  (`RuntimeResources.mm:281-286`), so an int4 pool can never adopt int8 cache
  entries or vice versa.

**The dangerous mismatch** — geometry saying 16,896 B/token while a kernel writes
int8 strides — cannot arise from a single source of truth here: the shader receives
only `data_bytes` and `scale_bytes` inside `SplashKvLayer`, both computed from the
same `Layout` that sized the buffer, and the kernel's element type is a template
argument chosen by the same `format`. A mismatched pair would need a host call site
that built a `Layout` with the wrong `format`; the only constructors that do so are
`loaded.targetKvLayout(config.kvFormat)` and `kvLayout.format = config.kvFormat`
(`RuntimeResources.mm:436`), both from the one CLI value. `Runtime.cpp:400` then
asserts the pool's layout matches the model's for the pool's own format.

## 3. Nibble ownership — the argument

**Layout.** For one tensor of one page, element `e(h, t, d)` of INT4 is
`splash_kv_key_element(h, t, d) = (h*32 + t)*256 + d` with `h < 4`, `t < 32`,
`d < 256`, and element `e` lives in byte `floor(e/2)`, `e` even in the low nibble.
The same formula serves keys and values (`splash_kv_value_index_of`).

**Claim 1 — a byte's two elements are one token's neighbours.** For fixed `(h, t)`,
`d = 2k` and `d = 2k+1` map to `e = base + 2k` and `base + 2k + 1`, where
`base = (h*32 + t)*256` is even. Both have `floor(e/2) = base/2 + k`. No other `d`
maps there, since `e` is strictly increasing in `d`.

**Claim 2 — no two threadgroups share a byte.** A row `(h, t)` occupies elements
`[base, base + 256)`, i.e. bytes `[base/2, base/2 + 128)`. Rows are laid out at
`base = 128 * (h*32 + t)` elements, so their byte ranges are the 128-byte blocks
`[64*(h*32+t), 64*(h*32+t) + 128)` — a partition of `[0, 16384)`, disjoint across
`(h, t)` and therefore across KV heads. The store dispatches one threadgroup per
`(tensor, head, token)` row (`splash_store_chunk_phase`: `rows = chunk_tokens*KVHeads`,
`row = group % rows`, `head = row / chunk_tokens`, `token = row % chunk_tokens`), so
two elements of one byte always belong to the *same* threadgroup.

**Claim 3 — within the threadgroup, one lane writes.** Thread index is the dimension
(`thread_index < 256`), so the two elements of a byte are threads `2k` and `2k+1`:
same simdgroup (`2k/32 == (2k+1)/32`), adjacent lanes. `paged_store_row.h:100-110`
has `neighbour = simd_shuffle_down(quantized, 1)` and writes only when
`dimension % 2 == 0`, packing `own | neighbour << 4`. Exactly one store per byte, no
atomics, no read-modify-write, and no dependence on whether the neighbour's own store
has landed.

**Claim 4 — slab starts are byte-aligned.** `SplashKvAddressing::page` computes
`key_element/2` and `value_element/2`. A head's slab starts at
`page*16384 + h*8192` elements (keys, and values because they are token-major),
`h*8192` even; the values region base is `extent_pages*(data + scale)` =
`128*16,896` bytes, even; so every division is exact and no head starts mid-byte.
Had INT4 kept INT8's dimension-major values, a value row would start at
`(h*256 + 0)*32 + t` — its tokens would be *consecutive elements*, so tokens `2j`
and `2j+1` of one dimension would share a byte in **different threadgroups** with no
ordering between them. That is the defect the token-major layout removes, and
`int4_kv_test.cpp` counts those cross-threadgroup pairs (it requires every one of the
4·16·256 pairs to be cross-threadgroup in the dimension-major layout, so the
distinction cannot silently disappear).

## 4. Memory arithmetic to 262,144

Constants from the code: page = 32 tokens; int4 page = 540,672 B; int4 extent
alignment = 128 pages; `speculativeScratchTokens = 7`; model maximum = 262,144
(`ExecutionGeometry.h:8`).

Engine plan (`MemoryPlan.cpp`): `pages = (dynamicBudget - laneState) / pageBytes`;
`extentPages = Layout::extentPagesFor(pages)`; `capacityPages = pages - pages %
extentPages`; `tokens = capacityPages * 32`; context = `min(262,144, tokens - 7)`.

The engine's own int8 figures pin the budget without loading the model:

| int8 context | capacity pages | budget for one request's KV |
|---|---|---|
| 163,833 (with ANE split) | 5,120 (= 163,840 tokens) | [5,452,595,200, 5,453,660,160) B = 5.078 GiB |
| 180,217 (no ANE split) | 5,632 (= 180,224 tokens) | [5,997,854,720, 5,998,919,680) B = 5.586 GiB |

At int4 on the *smaller* of those budgets: `5,452,595,200 / 540,672 = 10,084` pages,
`extentPagesFor` picks 256 (the multiple of 128 nearest 128 MiB with no remainder),
capacity `9,984` pages = `319,488` tokens → context `262,144` (clamped, with 57,344
tokens of headroom). On the larger budget: 11,093 pages → 11,008 capacity pages →
352,256 tokens → also 262,144.

What 262,144 tokens actually cost: `ceil(262,151/32) = 8,193` pages
= **4,429,725,696 B = 4.126 GiB** at int4, against 8,725,217,280 B = 8.126 GiB at
int8 — the full context fits in less than the int8 budget that only bought 163,833.
A 180,224-token history costs `5,632 * 540,672 = 3,045,064,704 B = 2.836 GiB` at
int4, versus 5.586 GiB at int8.

So the claim "int4 reaches 262,144 on the automatic 24 GiB budget" is arithmetically
necessary given the int8 measurements, not merely plausible: it needs 4.13 GiB where
int8 already had 5.08 GiB. `int4_kv_test.cpp::testContextArithmetic` re-derives this
from `Layout` for every budget in both intervals.

Not verified here: the ~6 % decode cost. It comes from the earlier measurement in
`FLAG_AUDIT.md` / `OPTIMIZATION_REPORT.md`; re-measuring needs the 27B model, which
this audit was told not to load. Nothing in the code contradicts it — int4 adds one
`simd_shuffle_down` and one `rint` per pair in the store, and the decode-side cost is
the `int4b_format` matmul path — but treat the number as carried over, not re-proven.

## 5. Evidence that the kernels are right

New host test — `dev/tests/engine/int4_kv_test.cpp`, target
`build/engine-tests/int4-kv`, wired into `dev/native.mk` (variable, prerequisite with
`$(ENGINE_LIBRARY)`, link group, `test-engine-cpu` recipe). It covers: page geometry
and ABI region offsets; slab alignment; the nibble-ownership enumeration of §3;
a byte-level store→read round trip of a full page of both tensors (codes, ordering,
adjacency, zero rows); `Layout → KvPool` page charging and extent size; the 262,144
arithmetic of §4; and the plan-to-kernel-name mapping for both geometries.

GPU test — `paged_attention_plan_test.mm` now also runs `kv::Format::Int4` through
the production `addPrefillStore`/`addPrefill`/`addVerify` encoders and compares the
result with an independent scalar softmax over the exact stored pages, at
`q=24/4 KV heads` and `q=16/2 KV heads`. Measured on this machine:

```
MTL_SHADER_VALIDATION=1 ./build/engine-tests/paged-attention-plan build/splash.metallib --long int4
long attention: format=int4 q=24 history=131072 PASS
long attention: format=int4 q=24 history=180224 PASS
long attention: format=int4 q=24 history=260096 PASS
long attention: format=int4 q=16 history=131072 PASS
long attention: format=int4 q=16 history=180224 PASS
long attention: format=int4 q=16 history=260096 PASS
long paged attention plans: PASS
```

(`--long` gained an optional format argument so one format's sweep can run alone.)

Both tests were checked against injected regressions:

* `prefillSplitPipeline` returning the int8 name for `Int4` → `int4_kv_test` fails
  with "an INT4 prefill plan does not run the INT4 split kernel".
* `dataBytesPerLayerPage()` returning int8's bytes for `Int4` → compile-time failure
  of the geometry assertions.
* `splash_kv_value_index_of` always using the dimension-major index (i.e. int4
  silently keeping int8's value layout) → the GPU test fails immediately:
  `attention candidate differs from baseline: history=33 rows=7
  maximum_absolute_error=0.136 cosine=0.850`.

The last one is the important one: the silent-degradation mode named in the brief is
caught, not just asserted.

## 6. Defects found

**Fixed (reporting, no numerical effect).**

1. `kv::storageFormatName(Format::Int4)` returned
   `q4s4_f32_scale_per_token_head_k_token_major_v_dimension_major`. Values are
   token-major in int4 alone; the string is what the status JSON publishes as
   `identity.cache.dtype` and is the label a future reader would trust. Now
   `..._k_token_major_v_token_major`, with the reason in a comment.
2. `Status.cpp` reported `"quantization": "none"` and `"scale_type": "none"` for any
   non-int8 format, so a running int4 server claimed its KV had no quantization and
   no scales. Now `symmetric_int4` / `float32` for int4 (bf16 keeps `none`), and
   `"value_layout"` reports `token_major` for int4 instead of the hard-coded
   `dimension_major`.

**Documented, not changed — dead int4 kernels that contradict the live layout.**

`runtime/metal/kernels/shared/kv_int4.metal` (`quantize_kv_int4`,
`dequantize_kv_int4`, `int4_paged_attention`) and
`runtime/metal/kernels/ops/int4_quantize.metal` (`quantizeKvInt4`,
`dequantizeKvInt4`) are compiled into `build/splash.metallib` and dispatched by
nothing (`grep` over `runtime/`, `server/`, `dev/` finds no host reference). They
implement a *different* format: `[token][head][dim/2]` with a zero point, page-major
`(page*heads*page_size + ...)`. `int4_paged_attention`'s value addressing
(`value_base = (page*heads*page_size + h*page_offset) * (dim/2)`, line 167) also drops
the `page_size` factor for `h`, so it is wrong on its own terms. They are not on any
path, but they are the first thing a reader finds by name. Removing them is a
one-line Makefile-independent delete and is recommended as a separate commit; this
audit left them so the change stays reviewable.

**Pre-existing, unrelated to int4.** At `c8b5d3dc`, `make test-engine-cpu` cannot
link `build/engine-tests/engine-cache` or `.../engine` (`KvCache.cpp` calls
`AsyncKvSwap`, which `dev/native.mk`'s `CACHE_SOURCES` never compiles), and
`gguf-planner` fails 32 cases. Both reproduce in a pristine worktree of the same
commit. `make -j8` itself is clean (0 errors, 0 warnings) and the int4 targets build
and pass.

**Adjacent, not KV:** `--draft-format int4|fp32` is parsed into
`NativeArguments::draftFormat` and read by nothing. It is the draft *weights* format,
not KV, so it does not affect this audit — but by the "a flag either works or is not
in the CLI" rule it is currently decoration.
