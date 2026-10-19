# Splash — current status

This is the single authoritative statement of what the engine does today, what
was measured, and what is settled. It replaces every `PHASE*.md`, `*_REPORT.md`
and `WEEK*.md` in the repository root; those are archived under
[`docs/archive/`](docs/archive/) and are historical only. Where an archived file
disagrees with this one, **this file is correct**.

Scope: the C++/Metal runtime in `runtime/` plus the Python server in `server/`.
Last verified against the tree at the commit that added this file.

---

## 1. What the engine does today

Splash serves local inference for three model families on Apple silicon:

| Target | Weights | Formats |
| --- | --- | --- |
| Qwen3.8-27B | dense | GGUF (Q4_K…Q8_K, mixed), MLX affine/mxfp4/nvfp4, NVFP4 |
| Qwen3.6-35B-A3B | sparse MoE | same |
| Prism ML Ternary-Bonsai-2-27B | dense, ternary | PQ2_0 (rotated), PTQ1_0 (rotated), F16 |

Serving path: `server/server.py` starts `build/splash serve`, which owns the
runtime. Requests are decoded by a DFlash2 draft plus a batched verify pass
(speculation is **always on**; there is no non-speculative decode path), with a
paged KV cache that can be quantized (int8 default, int4 and bf16 optional),
tiered to SSD, and reused across requests by prefix match.

The product-facing description is [`README.md`](README.md); build, API and
internals are [`DEVELOPMENT.md`](DEVELOPMENT.md). This file covers **measured
behaviour and retracted claims**, not usage.

---

## 2. Measured performance

### 2.1 The reference measurement

Everything below is one configuration, quoted together because the numbers are
meaningless apart from it.

| | |
| --- | --- |
| Hardware | M5 Pro, 24 GiB unified memory |
| Model | `prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0` |
| Launch path | Python server (`server/server.py` → `build/splash serve`) |
| Sampling | greedy |
| Prompts | 256 tokens |
| KV | int8 (default) |

| Metric | Value |
| --- | ---: |
| Marginal decode | **39.4 tok/s** |
| Time to first token | **116 ms** |
| Context | **163,833 tokens** |

"Marginal decode" is the steady-state rate after the first token, not a
whole-request average.

**These three numbers are the only ones safe to quote for this model.** Older
figures of **62.9 tok/s**, **107 tok/s**, 72.6 tok/s, 84.8 tok/s and 30–36 tok/s
that appear in archived reports came from different launch paths (the
`serve-native` binary driven directly instead of through the Python server front
end), different memory configurations, or single un-repeated runs on a thermally
drifting machine. They are **not** comparable to the row above and must not be
presented as improvements or regressions against it. If you need a number,
re-measure it on the same path.

### 2.2 Reproducing it

The Python front end is the launch path; it builds a `serve-native` command line
and execs it. Running that command by hand is a *different* configuration and
produces a different number — this is the single most common source of bad
figures in the archive.

```bash
./splash serve --model prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0
```

Then send one greedy completion of a 256-token prompt to
`/v1/chat/completions` and read the `metrics` block of the response: it reports
`prefill`, `decode`, `ttft_ms` and `stream_tokens_per_second` separately, which
is what makes "marginal decode" and "TTFT" separable at all. Context comes from
`/v1/models` for the same server, not from a second launch.

First run after idle is always the fastest; the spread on an identical
configuration has been measured at 32.33–37.31 tok/s. Repeat before believing a
difference.

### 2.3 Decode is weight-bandwidth-bound

This is the single most useful fact about the engine's performance, and it
explains why most "optimisations" in the archive did nothing:

- One decode cycle streams **7.93 GiB** of weights (6.75 GiB target + 1.18 GiB
  draft).
- Sustained bandwidth is **~111 GB/s** on this machine, which puts the floor at
  ~77 ms per cycle.
- Measured cycle time is **flat at 74.8–76.5 ms for 8, 16 and 32 verify rows**.
  Quadrupling the rows does not lengthen the cycle.

Consequences, all measured:

- **Batch width, not draft width, is the aggregate-throughput lever.** Aggregate
  decode went 32.7 → 64.2 → 143.5 tok/s at 1 → 2 → 4 lanes while cycle time
  stayed ~75 ms.
- **Speculation is worth ~×2.5** (2.5 tok/cycle at 76.5 ms ≈ 32.7 tok/s, versus
  1.0 tok/cycle without it). Acceptance is **18–27%**, and that is not waste:
  the rows are nearly free. Narrowing the draft to raise acceptance was measured
  and costs about **−40%** throughput.
- **Any change that does not reduce bytes read per token cannot raise decode
  speed.** This is why the format and prefetch work below produced no decode
  gain, and why that is a structural result rather than a failed tuning effort.

### 2.4 Context

Context is a function of the memory plan, so it is only meaningful with its
configuration. At the reference configuration above it is **163,833 tokens** at
int8 KV. Larger contexts are reachable by moving bytes out of the hot path
(`--vision-text-only`, `--state-format bf16`, `--kv-format int4`,
`--max-cache-disk`), and each of those trades something; see §4.

---

## 3. Formats

### 3.1 PQ2_0 and PTQ1_0 are the same weights

Both are Prism ML's **rotated** ternary formats. They decode to bit-identical
weights: element-wise max |diff| = **0.0** across all 17,408 rows of
`blk.0.ffn_gate.weight`. PQ2_0 uses only codes 0/1/2 (measured 33.93 / 32.79 /
33.28%), i.e. the same ternary checkpoint at group 128.

### 3.2 PTQ1_0 is 28 bytes per 128 ternary weights

| Property | Value |
| --- | --- |
| GGML type | **143** |
| Runtime format id | `GGUF_FMT_PTQ10` = **43** |
| Block | **28 bytes / 128 weights** (1.75 bpw) |
| Layout | `qs[24]` (24 base-3 codewords × 5 trits = 120) + `qh[2]` (2 × 4 = 8) + fp16 `d` at offset **26** |
| Dequant | `weight[e] = d * trit[e]`, `trit ∈ {−1, 0, +1}` |

It is **not** 18 bytes and **not** 1-bit. Both errors are retracted; the 18-byte
figure came from a wrong entry in `QuantFormat.h` that truncated every PTQ1_0
tensor, and the 1-bit figure came from misreading the 2-bit runtime codes as
binary.

### 3.3 What PTQ1_0 buys

A **storage and load-time** win, and nothing else. Measured on the two Prism
GGUFs of the same checkpoint, same machine, same launch path:

| Metric | PQ2_0 | PTQ1_0 | Delta |
| --- | ---: | ---: | ---: |
| Weights on disk | 6.90 GB | 5.95 GB | **−13.8%** |
| Weights load | 3.21 s | 2.80 s | **−12.8%** |
| Decode | — | — | **none** |

**No decode gain, by construction.** The repack decodes PTQ1_0's base-3 trits
into exactly the 2-bit codes PQ2_0 stores. Both formats therefore have identical
*runtime* geometry — `plane0` 8 bytes per 32-element group (32 B per
128-element block) plus one fp16 scale per block, 34 B per block in memory:

| | Native block on disk | Runtime bytes per 128 weights |
| --- | ---: | ---: |
| PQ2_0 | 34 B | 34 B |
| PTQ1_0 | **28 B** | **34 B** |

The saving exists only on disk. Identical bytes per cycle on a bandwidth-bound
decoder means identical tok/s. Do not expect a speedup from PTQ1_0, and do not
trust any report that claims one — the apparent +20% in an early draft was
thermal drift, reproduced as 37.31 and 32.33 tok/s for the *same* configuration
on one server.

---

## 4. Flags

Only flags with a measured effect are listed first; the rest of the parsed
surface is documented in `DEVELOPMENT.md`.

### 4.1 Do what they say

| Flag | Effect |
| --- | --- |
| `--working-set` | Sets the Metal budget the memory plan sizes context from. |
| `--vision-text-only` / `--language-only` | Skips the vision tower: **+28,672 tokens** of context at the configuration where it was measured (180,217 → 208,889). |
| `--state-format bf16` | Halves recurrent-state bytes; buys context. |
| `--kv-format int4` | Halves KV bytes per token (33,280 → 16,896); roughly doubles context at a fixed budget, at ~6% slower decode on long requests. |
| `--kv-defragmentation` | Compacts pages stranded in partially used extents. Context-neutral; only matters under fragmentation. |
| `--max-cache-disk` | Caps the SSD cache (default auto, up to 32 GiB; `0` disables). |
| `--persistent-cache` | Keeps the SSD cache across restarts. |
| `--ane` / `--disable-ane` | Turns the ANE FFN split on/off. Irrelevant for the ternary model — see §5. |
| `--ane-telemetry` | ANE utilisation counters. |

### 4.2 Inert — present in the parser, no effect

| Flag | Reality |
| --- | --- |
| `--ane-layers` | **Requests only; selects nothing.** Parsed, validated against the layer count, logged (`describeHybridConfig`) and counted in ANE telemetry — but `loadQwen3_8Weights` takes the config and discards it (`(void)hybridConfig; // TODO`), and `GgufImage` reads each layer's format from the GGUF. It cannot move a layer onto the ANE, and every layer of this model is declined anyway (§5). |
| `--enable-weight-prefetch`, `--prefetch-layers` | **Placebo.** `engine/WeightPrefetch.cpp` walks a *resident* Metal buffer from a CPU dispatch queue (`dummy += ptr[offset]`). `WeightSource` reads with `pread` + `F_NOCACHE` and there is **no `mmap` anywhere in `runtime/`**, so there is nothing to fault in and nothing to read ahead into. Cost is ~0.2 ms/layer of pure overhead. Off by default; leave it off. |
| `--kv-swap-*` (`AsyncKvSwap`) | Runs, but **cannot extend context.** It has no handle to the KV pool: it can copy a page to its swap file and mark it `swapped` in its own table, but it cannot release that page's RAM, so the memory plan does not grow. Only the cache's own demotion path (`Cache::demoteKv` → `KvCache::dropPage`) returns a page's RAM, and that path does not go through AsyncKvSwap. |
| `--fused-attention`, `--persistent-ane-calibration` | **Dead.** Parsed into the argument struct, never copied to the runtime config. |
| `--speculation-threshold` | Reaches `Runtime.cpp` and adjusts per-request acceptance bounds, but speculation is always on and the 8 verify rows are a compile-time ABI constant. It cannot turn speculation off or change its width. |

### 4.3 Removed, and why

| Flag | Why it is gone |
| --- | --- |
| `--write-coalescing` | Broke demotion: a buffered `transfer` was dropped before `demotions_`/`setTransferring()`, and `forceFlush()` was never called. Also structurally inapplicable — a KV slot is one 1040 KiB page written whole. |
| `--kernel-fusion` | The kernels existed but nothing dispatched them, and the fused RMSNorm+Linear kernel was *wrong* (`normed[i] · Σ W[i][j]` instead of `Σ normed[j] · W[i][j]`, fp32 instead of bf16). Both fusions it promised are now always on and correct in the normal path. |
| `--speculative-decoding` | Nothing to enable: the loop body was empty, and DFlash draft + 8 verify rows run unconditionally. |
| `--int4-kv-cache` | Dead: `setInt4Cache()` never called, kernels never dispatched, and `dataBytesPerLayerPage()` counted int4 as 2 bytes/element — i.e. *larger*. The working route is `--kv-format int4`. |

---

## 5. The Neural Engine: closed for the ternary model

**ANE acceleration is impossible for the Prism ternary family.** This is a
structural result, not a pending task. The chain:

1. `AneFfn` applies its **own** Hadamard rotation to inputs, weights and
   intermediate rows — `rotationSigns()` in `runtime/ops/AneFfn.cpp`, signs drawn
   from `std::mt19937(20260930)`, materialised as a grouped 1×1 convolution
   (`rotationBlob`).
2. The split therefore needs weights in the **unrotated** basis.
   `Projection::takesPlaneViews()` (`runtime/ops/Linear.cpp`) requires
   `!rotation` — the one exception carved out for PTQ1_0 is a bug, not a
   loophole, and is recorded in §7 — and `AneFfn::shapeOf` builds its operands
   through `leadingRows`/`leadingInputs`, which call `requirePlaneViews`.
3. **Both** Prism formats are rotated. PQ2_0 and PTQ1_0 decode to bit-identical
   weights, and PTQ1_0's tensors are exactly the set the GGUF's
   `prism.hadamard.*` keys name. No unrotated quantized variant of this
   checkpoint exists: the repository publishes only PQ2_0, PTQ1_0 and F16.
   Q4_K_M is not published, so the hybrid plan had no unrotated weights to load.
4. Un-rotating offline is impossible: the checkpoint is ternary **in the rotated
   basis**, and its inverse rotation is dense — it cannot be carried in a 2-bit
   code without becoming a different, larger model.

At runtime the split declines and says so:

```
Warning · Neural Engine FFN split unavailable
(views of a projection's planes take one unrotated quantized tensor, not a view)
```

`--ane-layers` only *requests* layers; as noted in §4.2 nothing selects a format
from it, and even if something did, the split would decline every layer of this
model. ANE is real and used for **Qwen** targets, whose GGUFs carry unrotated
Q4_K.

---

## 6. Closed questions

Do not re-open any of these without new evidence. Each was investigated,
measured, and written up in an archived report; the conclusion is repeated here
so the conclusion is what survives, not the report.

| Claim | Verdict |
| --- | --- |
| ANE hybrid quantization gives +30% decode / 35–40% ANE utilisation | **False.** ANE is unreachable for this model (§5). No hybrid run ever produced a decode gain. |
| ANE hybrid is "95% complete, ready for testing" | **False.** The infrastructure existed; the premise did not. Nothing was ever going to run on the ANE. |
| PTQ1_0 needs an 18-byte block | **False.** 28 bytes / 128 weights (§3.2). |
| PTQ1_0 is a 1-bit binary format | **False.** Ternary base-3 codewords, 2-bit runtime planes (§3.2). |
| PTQ1_0 speeds up decode | **False.** Identical plane geometry → no decode change; storage/load win only (§3.3). |
| WeightPrefetch gives 8–10% decode latency | **False.** No demand-faulted mapping exists; it touches resident buffers. ~0.2 ms/layer of cost (§4.2). |
| AsyncKvSwap extends context to 1–2M tokens | **False.** A swap-out cannot return a live request's RAM to the pool, so the plan never grows. The working SSD-backed context path is `KvPageTier`/`ExtendedKvTier`, selected by `--ssd-kv` + `--max-context-extended` and budgeted by `--max-cache-disk` (§4.2). |
| Hybrid GGUF (PTQ1_0 + PQ2_0 per layer) is worth shipping | **False.** Both formats are the same weights at the same runtime width; mixing them changes nothing but load complexity. |
| Narrower draft / adaptive draft width helps | **False.** Measured ~−40% (§2.3). |
| Multi-page KV slots would cut write cost | **No gain.** Same bytes, only fewer syscalls; costs a disk format change plus cache migration. |
| Baseline is 62.9 or 107 tok/s | **Not comparable.** Different launch paths/configs. The reference is 39.4 tok/s as specified in §2.1. |

---

## 7. Known stale code

Recorded here so nobody trusts them. **Not** fixed in this commit, which is a
documentation change.

1. **`runtime/ops/Linear.cpp`, `Projection::takesPlaneViews()`** — special-cases
   PTQ1_0 with `(!rotation || isPTQ1_0)` and a comment calling PTQ1_0
   "unrotated binary quantization". **That is false** (§3). The exception was
   added in `8cd7d16a` to let the ANE accept PTQ1_0 and was never reverted,
   although [`docs/archive/PHASE2_VALIDATION_REFACTOR.md`](docs/archive/PHASE2_VALIDATION_REFACTOR.md)
   says it was. Left in place it would
   let the split consume rotated weights and compute garbage; the correct test
   is the plain `!rotation`. **Treat this as an open bug, not a design choice.**
2. **`runtime/ops/AneFfn.cpp`, `AneFfn::unsupported()`** — comment says plane-view
   validation is "deferred … runtime validation occurs during layer
   construction". Validation does happen, in `requirePlaneViews`, but only
   because construction goes through `leadingRows`; the comment invites the
   reader to believe the split checks nothing.
3. **`runtime/model/WeightStore.cpp:152`** — points at
   `docs/archive/PHASE2_COMPLETION_REPORT.md` for "implementation options".
   Those options are all dead; the path is kept only so the reference resolves.
   §5 is the answer.

---

## 8. History

Every superseded phase, week and completion report now lives in
[`docs/archive/`](docs/archive/). Each carries a header of the form

```
> SUPERSEDED by STATUS.md — <what in this file is now known to be wrong>
```

They remain useful for reconstructing *why* a dead end was tried. They are not
evidence about current behaviour, and their numbers are not comparable to §2.1.
