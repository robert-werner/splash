> SUPERSEDED by STATUS.md — nothing material is wrong here - it is the reverse-engineered 28-byte layout that STATUS.md relies on; archived only to keep one authoritative document at the root

# PTQ1_0 (GGML type 143) block layout — reverse-engineered and verified

Source: `Ternary-Bonsai-2-27B-PTQ1_0.gguf` (5,946,648,928 bytes), cross-checked
element-by-element against `Ternary-Bonsai-2-27B-PQ2_0.gguf`.
Tensor studied: `blk.0.ffn_gate.weight`, dims `[5120, 17408]` (ne0 = 5120 = fastest /
columns, 17408 rows, 40 blocks per row).

## TL;DR

`block_ptq1_0` is **28 bytes per 128 weights** (1.75 bpw) — not 18. It is upstream
`TQ1_0`'s base-3 trit packing with Prism's group size 128 (upstream TQ1_0 is group 256).

```
struct block_ptq1_0 {   // 28 bytes = 128 ternary weights
    uint8_t  qs[24];    // bytes  0..23  24 base-3 codewords x 5 trits = 120 weights
    uint8_t  qh[ 2];    // bytes 24..25   2 base-3 codewords x 4 trits =   8 weights
    ggml_half d;        // bytes 26..27  IEEE fp16 scale, little-endian
};
```

`weight[e] = d * trit[e]`, `trit[e] ∈ {-1, 0, +1}`. No zero-point, no bias, no tiling:
blocks sit in plain row-major order, 128 consecutive columns each, row by row
(`block index = row * (columns/128) + column_block`). `d = amax` of the block, exactly.

## Why 18 bytes was wrong

`runtime/metal/abi/QuantFormat.h:198` declares `{143, 128, 18, 4, 0, 2, 4, "ptq10"}`, and
`scripts/hybrid_gguf_converter.py` repeats it as `GGML_TYPE_PTQ1_0: (128, 18)`. Both are
wrong by 10 bytes, so every PTQ1_0 tensor is read truncated (converter computes
12,533,760 B for `blk.0.ffn_gate.weight`; the file holds 19,496,960 B) and every later
tensor is read at a wrong offset.

Evidence from the file:
- For all **402** type-143 tensors, `next_tensor.offset − offset == n_elements/128 * 28`
  exactly (0 exceptions). The last tensor ends exactly at EOF (5,946,648,928).
- Comb search: fp16 values at even offsets inside the plausible scale band
  (0.004 … 0.045) land overwhelmingly at `pos % 28 == 26` — 447,634 hits vs ~23,800 for
  every other even residue. At period 18 there is no structure at all (every in-block
  offset gives ~27 % "plausible", i.e. chance), which is exactly what the earlier
  18-byte experiments measured.

## Codeword encoding (the non-obvious part)

Each `qs` byte is **not** a plain base-3 number 0…242. The encoder rescales a 5-trit
code `q ∈ [0, 243)` to a byte with `ceil(q·256/243)`, so the 243 codewords are spread
evenly over 0…255 and 13 byte values are never used (1, 20, 40, 60, 79, 99, 119, 138,
158, 178, 197, 217, 237 — confirmed absent from 1.4 M real bytes). Decoding therefore
uses a multiply-and-shift digit extractor rather than `% 3`:

```c
static const uint8_t pow3[6] = {1, 3, 9, 27, 81, 243};
uint8_t  q  = (uint8_t)(codeword * pow3[n]);   // 8-bit wrap
int16_t  xi = ((uint16_t) q * 3) >> 8;         // xi ∈ {0,1,2}
float    v  = (xi - 1) * d;                    // → {-d, 0, +d}
```

Verified: for all 243 codes, `decode(rescale(q)) == q` digit-for-digit, and 0 duplicate
digit tuples. Using plain base-3 (`b / 3^n % 3`) or masking a codeword to 242 is wrong.

**Trit order inside a byte is most-significant-first**: digit index `n` weights `3^(4−n)`
for `qs` and `3^(3−n)` for `qh` (the encoder builds `q = 3q + xi`). `qh` packs only 4
trits and has no leading digit, i.e. it is `q·3` before rescaling.

## Element order (not byte-major)

The 24 `qs` bytes are consumed in **stages of 32, 16, 8**; each stage emits its digits
with the byte index varying fastest and the digit index slowest. With only 24 bytes the
32-byte stage never fits, so the real sequence is 16 bytes then 8 bytes:

```
j = 0
for c in (32, 16, 8):
    while j + c <= 24:
        for n in 0..4:          # trit digit (most significant first)
            for m in 0..c-1:    # byte within the stage
                emit digit(qs[j+m], n)
        j += c
for n in 0..3:
    for h in 0..1:
        emit digit(qh[h], n)
```

i.e. `qs[0..15]` → weights 0..79, `qs[16..23]` → weights 80..119, `qh[0..1]` → weights
120..127. A plain byte-major walk (all 5 digits of byte 0, then byte 1, …) produces the
same multiset but scrambles column order, which silently destroys the row.

## Reference implementation

```python
import numpy as np

_POW3 = (1, 3, 9, 27, 81, 243)

def dequantize_ptq1_0(raw_bytes, n_elements):
    """GGML type 143 (PTQ1_0): 28-byte blocks of 128 ternary weights.

    raw_bytes : bytes-like, at least (n_elements // 128) * 28 bytes
    n_elements: number of weights (multiple of 128)
    returns   : list[float] in row-major element order
    """
    if n_elements % 128:
        raise ValueError("n_elements must be a multiple of 128")
    nblocks = n_elements // 128
    if len(raw_bytes) < nblocks * 28:
        raise ValueError("raw_bytes too short: %d < %d" % (len(raw_bytes), nblocks * 28))
    out = []
    for i in range(nblocks):
        blk = raw_bytes[i * 28:(i + 1) * 28]
        d = float(np.frombuffer(blk[26:28], dtype='<f2')[0])
        qs, qh = blk[0:24], blk[24:26]
        v = []
        j = 0
        for c in (32, 16, 8):                 # stage widths; with 24 B -> 16, then 8
            while j + c <= 24:
                for n in range(5):            # trit digit, most significant first
                    p = _POW3[n]
                    for m in range(c):        # byte within the stage
                        v.append(((((qs[j + m] * p) & 0xFF) * 3) >> 8) - 1)
                j += c
        for n in range(4):                    # qh: 4 trits per byte x 2 bytes
            p = _POW3[n]
            for h in range(2):
                v.append(((((qh[h] * p) & 0xFF) * 3) >> 8) - 1)
        assert len(v) == 128
        out.extend(x * d for x in v)
    return out
```

Vectorised form (what the statistics below used): build `(rows, blocks_per_row, 128)`
int8 trits with the same index walk, then `w = trits * d[..., None]`.

## Verification against PQ2_0

PQ2_0 (`block_pq2_0`, 34 B/128) decodes as: `d = fp16 at block offset 0`, then 32 bytes
of 2-bit codes, 4 per byte, low bits first, `value = d * (q − 1)`, `q ∈ 0..3`.
Note the `FmtPQ20` comment in `quant_formats.h` says `d` sits at the end — it is at the
**start** (offset 0..1); reading 32..33 yields only 33.5 % plausible scales.

`blk.0.ffn_gate.weight`, per-row L2 norm (17408 rows × 5120 columns):

| row | PTQ1_0 L2 | PQ2_0 L2 | rel diff |
|----:|----------:|---------:|---------:|
| 0 | 0.717478514 | 0.717478514 | 0.0000000 % |
| 1 | 0.650170386 | 0.650170386 | 0.0000000 % |
| 2 | 0.676545441 | 0.676545441 | 0.0000000 % |
| 3 | 0.658903301 | 0.658903301 | 0.0000000 % |
| 4 | 0.607357323 | 0.607357323 | 0.0000000 % |
| 5 | 0.628401399 | 0.628401399 | 0.0000000 % |
| 6 | 0.638180614 | 0.638180614 | 0.0000000 % |
| 7 | 0.661417842 | 0.661417842 | 0.0000000 % |

- All 17408 rows: max relative norm difference **0.000e+00 %**, corr 1.000000000000.
- **Element-wise** max |PTQ1_0 − PQ2_0| = **0.0** for rows 0..7, and for
  `blk.0.attn_qkv.weight` rows 0, 1, 777 (norms 1.11920762 / 1.11302471 / 1.08685231,
  diff 0.0). The two packings reconstruct identical float weights, not merely equal norms
  — both are the same ternary checkpoint at group 128, differing only in code width.
- Decoded trits equal PQ2_0's `code − 1` for every element of every row checked; per-block
  scales `d` are byte-identical between the two files.
- Trit histogram: −1 → 33.59 %, 0 → 32.77 %, +1 → 33.64 %.
- Scales: all 696,320 finite, 0.007267 … 0.029037, mean 0.011226.
- Weights: |w| ∈ [0, 0.029037], mean |w| 0.007547; row norms 0.526 … 1.472 (mean 0.660).
- `d == amax` of the block holds exactly: max |amax(decoded) − d| = 0.000e+00 over 2000
  random blocks (matches the encoder's `d = amax`).

## What needs fixing in Splash

1. `runtime/metal/abi/QuantFormat.h:198` — `{143, 128, 18, 4, 0, 2, 4, "ptq10"}` must
   become `{143, 128, 28, 2, 0, 2, 4, "ptq10"}`: 28 B/block, plane0 = 2 B per group of 32
   (26 payload bytes / 4 groups ≈ 2; exact packing is not group-of-32 aligned — see
   layout above), meta 2 B, `d` at block offset 26.
2. `scripts/hybrid_gguf_converter.py` — `GGML_TYPE_PTQ1_0: (128, 18)` → `(128, 28)`.
3. `FmtPTQ10` in `runtime/metal/kernels/common/quant_formats.h` — replace the 16-byte
   1-bit `d*(2q−1)` model with the base-3 codeword unpack above (26-byte payload, 5- and
   4-trit codewords, values in {−1, 0, +1}). The current struct cannot express 3 values
   per weight, so the payload must be decoded rather than bit-extracted per element.
4. `FmtPQ20` — `d` is at block offset 0, not 32; fix the comment and `loadMeta`.
