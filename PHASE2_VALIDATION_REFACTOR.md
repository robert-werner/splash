# Phase 2 — PTQ1_0 validation refactor

PTQ1_0 (GGML type 143, Splash format id `GGUF_FMT_PTQ10 = 43`) is accepted as a
normal **rotated** GPU quantized format, exactly like PQ2_0. It is *not*
unrotated: the layout reverse-engineering confirmed that PTQ1_0 decodes to
element-identical weights to PQ2_0, so `prism.hadamard.*` applies to it and the
loader must keep attaching `InputRotation` to its projections.

Nothing here touches the PTQ1_0 dequantization kernel (`FmtPTQ10`,
`block_ptq1_0`). That work is separate.

## What was wrong

1. `Builder::quantizedFfn` in `GgufImage.cpp` hard-coded a format per layer
   from the `--ane-layers` list: Q4_K/PTQ1_0 for ANE layers, PQ2_0 for GPU
   layers. A pure PTQ1_0 file therefore failed on the first GPU layer with
   `GPU layer blk.0.ffn_gate.weight expected PQ2_0 format, got 43`. The
   per-layer format selection the check assumed does not exist (Splash does not
   requantize at load); the GGUF decides every layer's format.
2. `requireRotation` in `GgufImage.cpp` insisted `token_embd.weight` be PQ2_0.
   PTQ1_0's token table is type 143, so a pure PTQ1_0 file was refused even
   after (1).
3. `Embedding::add` and the rotated gather kernel only knew PQ2_0
   (`gguf_embed_rotated_pq20`), so a rotated PTQ1_0 token table had no gather.
4. `Projection::takesPlaneViews` had been patched to treat PTQ1_0 as
   rotation-free. That silently corrupts weights (PTQ1_0 *is* rotated) and is
   reverted; the ANE FFN split now declines PTQ1_0 layers as it declines PQ2_0.

## Files and functions changed

### `runtime/model/GgufImage.cpp`
- `Builder::quantizedFfn(name, rows, columns)` — dropped the `isAneLayer`
  argument and the per-placement format requirement. It now accepts the three
  formats a dense FFN tensor may legitimately hold (PQ2_0, PTQ1_0, Q4_K) and
  names the offending tensor otherwise. ANE placement is no longer a load-time
  decision.
- `layerImage(...)` — dropped the `HybridQuantConfig` parameter and the
  `isAneLayer` lookup that fed `quantizedFfn`.
- `planImages(file, geometry)` — dropped the `HybridQuantConfig` parameter; the
  images no longer depend on it, so `loadModel` and `modelWeightBytes` cannot
  disagree about planned bytes.
- `requireRotation(...)` — the one token table may be `token_embd.weight` in
  PQ2_0 **or** PTQ1_0. The rest of the check (the rotation names exactly the
  repacked tensors, value heads grouped) is unchanged.

### `runtime/model/GgufImage.hpp`
- `planImages` declaration follows the signature above.

### `runtime/model/GgufTarget.{hpp,cpp}`
- `GgufTargetLoader` ctor no longer takes or stores `HybridQuantConfig`, and
  the now-unused `hybridConfig()` accessor is gone. `planImages` is called with
  the file and geometry only.

### `runtime/model/ModelFactory.{hpp,cpp}`
- `modelWeightBytes(root, descriptor, textOnly)` — dropped the
  `HybridQuantConfig` parameter (it no longer changes any image's size).
- `loadModel` still takes the config: it is what `--ane-layers` validates
  against the layer count, and the Gguf branch stops forwarding it to the
  loader.

### `runtime/engine/RuntimeResources.mm`
- The `modelWeightBytes` call site follows the new signature.

### `runtime/ops/Embedding.cpp`
- `Embedding::add` accepts a rotated token table in PQ2_0 **or** PTQ1_0 and
  dispatches `gguf_embed_rotated_<format name>` instead of the hard-coded
  `gguf_embed_rotated_pq20`.

### `runtime/metal/kernels/shared/gguf_rotation.metal`
- The rotated token gather is factored into a `template <class F>` helper
  `embed_rotated` (the threadgroup block is passed in, since Metal forbids
  declaring threadgroup variables in a non-kernel function) and instantiated
  twice: `gguf_embed_rotated_pq20` (unchanged behaviour) and
  `gguf_embed_rotated_ptq10`.

### `runtime/metal/kernels/common/gguf_embedding_formats.h`
- Added `GgufEmbedPTQ10`, the native-block decoder for a rotated token table's
  rows, alongside `GgufEmbedPQ20`.

### `runtime/metal/kernels/shared/embedding.metal`
- Registered `GGUF_EMBEDDING_FORMAT(ptq10, GgufEmbedPTQ10)`, giving
  `gguf_embed_ptq10` and `gguf_embed_f32_ptq10` for the unrotated path.

### `runtime/model/HybridQuantization.{hpp,cpp}`
- Comments state what `--ane-layers` actually does: it names the layers the
  user asks the ANE to take; the GGUF decides each layer's format, and the ANE
  FFN split accepts or declines each layer at runtime.
- `describeHybridConfig` no longer claims "N on ANE, M on GPU" (that split was
  never implemented); it reports the requested layers and says the formats
  come from the GGUF. `RuntimeResources.mm` logs it under the same
  `ANE Hybrid Quantization:` prefix as before.

### `runtime/ops/Linear.cpp`
- Reverted the PTQ1_0-is-rotation-free patch to
  `Projection::takesPlaneViews`; it is again
  `!planeInputs_ && one segment && !float && !rotation`. No debug output left.

## Verification (real runs, this machine)

- `make -j8`: clean, 0 warnings under `-Wall -Wextra -Werror`.
- `./splash serve --model prism-ml/Ternary-Bonsai-2-27B-gguf:PTQ1_0
  --kv-format int4 --vision-text-only`:
  - `Weights loaded in 3.10 s.`
  - `Ready · prism-ml/Ternary-Bonsai-2-27B-gguf:PTQ1_0 · context 256K ·
    language only`
  - completions return tokens: 59-token prompt at 262 tok/s prefill, 42 tok/s
    decode; a 4,559-token prompt prefills at 420 tok/s.
- `prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0` unchanged: loads in 3.28 s,
  `Ready · context 256K`, completions at 294 tok/s prefill, 100 tok/s decode.
- `install/models/ane-bench` with `--ane-layers 0,2,4,8,10,14` loads and logs
  `ANE Hybrid Quantization: 6 of 64 FFN layers requested (0,2,4,8,10,14); each
  layer's format comes from the GGUF.` The ANE FFN split then declines the
  layers with `views of a projection's planes take one unrotated quantized
  tensor, not a view` — correct now, because those layers are rotated PQ2_0 and
  PTQ1_0.
