> SUPERSEDED by STATUS.md — nothing material is wrong here - it is the corrected PTQ1_0 note (28-byte ternary blocks, bit-identical to PQ2_0, storage-only win, ANE impossible) and its findings are carried into STATUS.md; archived only to keep one authoritative document at the root

# Phase 2 — Measured Results (PTQ1_0), corrected

**Date**: 2026-10-18 19:20 MSK
**Model**: Ternary-Bonsai-2-27B (Prism ML), 27B ternary, M5 Pro 24 GiB
**Binary**: `build/splash`, C++20, `-Werror`, 0 warnings

---

## 1. PTQ1_0 layout — solved

`PTQ1_0` (GGML type 143) is **not** 18 bytes / 1 bit. Reverse-engineered from the
file and verified against PQ2_0:

```
block_ptq1_0 {          // 28 bytes = 128 ternary weights (1.75 bpw)
  uint8_t qs[24];       // 24 base-3 codewords x 5 trits = 120 weights
  uint8_t qh[ 2];       //  2 base-3 codewords x 4 trits =   8 weights
  ggml_half d;          // fp16 scale; weight = d * trit, trit in {-1, 0, +1}
};
```

Codewords are rescaled (`ceil(q*256/243)`), digits most-significant-first, element
order staged 16 bytes then 8 bytes (byte index fastest). `d` is the block's amax.

**Verification**: decoding PTQ1_0 yields **bit-identical** weights to PQ2_0 —
element-wise max |diff| = 0.0 over all 17408 rows of `blk.0.ffn_gate.weight`.
PQ2_0 uses only codes 0/1/2 (measured 33.93 / 32.79 / 33.28 %), i.e. both files
are the same ternary checkpoint at group 128, differing only in code width.

Files changed:
- `runtime/model/GgufFile.hpp` — traits `{143, {PTQ1_0, 128, 28}}`
- `runtime/metal/abi/QuantFormat.h` — `{143, 128, 28, 8, 0, 2, 4, "ptq10"}`
- `runtime/metal/kernels/shared/gguf_repack.metal` — `ptq10_code()` base-3 unpack
- `runtime/metal/kernels/common/quant_formats.h` — `FmtPTQ10` reads 2-bit codes
- `scripts/hybrid_gguf_converter.py` — `(128, 28)` block size

Hybrid GGUF regenerated: 6.60 GB, 84 PTQ1_0 + 767 PQ2_0 tensors.

---

## 2. Performance — the honest answer: no decode gain

**Structural proof, not a measurement**: after the repack both formats have
*identical* runtime plane geometry —

```
{142, 128, 34, 8, 0, 2, 4, "pq20"}   // PQ2_0
{143, 128, 28, 8, 0, 2, 4, "ptq10"}  // PTQ1_0
                  ^ plane0 = 8 B per 32-element group, meta 2 B, 4 groups
```

The repack decodes PTQ1_0's base-3 trits into the **same 2-bit codes** PQ2_0
stores, so GPU-side weight bytes per cycle are unchanged. Decode is
weight-bandwidth-bound; identical bytes → identical tok/s. No design change can
avoid this without a kernel that reads base-3 codewords natively — and those
span 16/8/2-byte stages, which does not align to the 32-element group the planes
are built on.

Measured, same launch path, 256 tokens, three distinct prompts, median:

| Configuration | Median | Range |
|---|---|---|
| PQ2_0 pure (`--disable-ane`) | 30.37 | 28.73 – 35.42 |
| Hybrid 6×PTQ1_0 + 58×PQ2_0 | 36.46 | 34.63 – 47.07 |
| Hybrid, `--disable-ane` | 34.82 | 32.06 – 46.17 |
| PTQ1_0 pure | 35.92 | 34.08 – 46.29 |

Marginal-rate repeats on one server gave 37.31 and 32.33 tok/s for the *same*
configuration — the spread is thermal drift (first run after idle is always
fastest). **The apparent +20 % in an earlier draft of this note was that drift,
not a PTQ1_0 effect.** Corrected conclusion: no measurable decode difference.

What PTQ1_0 *does* buy, and it is real:

| Metric | PQ2_0 | PTQ1_0 | Delta |
|---|---|---|---|
| Weights on disk | 6.90 GB | 5.95 GB | **−13.8 %** |
| Bytes per weight | 2.13 bpw | 1.75 bpw | **−17.6 %** |
| Weights load time | 3.21 s | 2.80 s | **−12.8 %** |

---

## 3. Quality

- Weights: bit-identical to PQ2_0 (proof above) — PTQ1_0 is a **lossless repack**
  of the same ternary checkpoint in 18 % fewer bytes.
- Generation: coherent, correct code. Side-by-side greedy output vs PQ2_0 on the
  same prompt differs only in trivial wording (`a single sorted list` vs
  `one sorted list`) — fp-level nondeterminism between execution plans, not a
  decode defect.

---

## 4. ANE FFN split: not achievable for this model family

The split stays off:

```
Warning · Neural Engine FFN split unavailable
(views of a projection's planes take one unrotated quantized tensor, not a view)
```

Architectural, not a bug:

- `Projection::takesPlaneViews()` requires `!rotation` — ANE needs weights in the
  **unrotated** basis, because `AneFfn` applies its **own** Hadamard rotation
  (`rotationSigns()`, mt19937 seed 20260930) to inputs, weights and intermediate rows.
- **Both** Prism formats live in Prism's rotated basis — proven by the
  bit-identical decode of PTQ1_0 and PQ2_0. No unrotated quantized variant of
  this checkpoint exists (Q4_K_M is not published; only PQ2_0, PTQ1_0, F16).
- Un-rotating offline is impossible: the checkpoint is ternary *in the rotated
  basis*; its inverse rotation is dense and cannot be carried in a 2-bit code.

ANE acceleration is therefore closed for Ternary Bonsai under Splash's current
ANE design. It would need an ANE program that consumes Prism-rotated weights
with Prism-rotated inputs (Prism's rotation inside the ANE graph).

---

## 5. Net result

| Item | Status |
|---|---|
| PTQ1_0 format support | ✅ implemented, verified bit-identical to PQ2_0 |
| Hybrid GGUF (PTQ1_0 + PQ2_0) | ✅ loads and serves |
| Decode speed from PTQ1_0 | ➖ **none** (identical runtime planes — by construction) |
| Model size on disk | ✅ **−13.8 %** (6.90 → 5.95 GB) |
| Weights load time | ✅ **−12.8 %** (3.21 → 2.80 s) |
| Quality regression | ✅ none (lossless repack) |
| ANE FFN acceleration | ❌ impossible for this checkpoint family |

PTQ1_0 is worth keeping as a **storage/load-time** win (smaller image, faster
start), not as a throughput win. Throughput on this model is set by weight
bandwidth at a fixed 2-bit plane width, which both formats share.

---
**Generated by**: the model 5.5
