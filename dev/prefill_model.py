#!/usr/bin/env python3
"""Analytic prefill cost model for the Qwen3.8-27B hybrid target.

Reads only a GGUF's *header* (tensor table and metadata), never the weights, so
it runs beside a live server. From that it derives

  * the layer census (which blocks are full attention, which are GDN),
  * the parameter count each layer multiplies in prefill,
  * the kernel dispatches one 2048-row chunk encodes per layer kind,
  * matmul and attention FLOPs as functions of prompt length,
  * what the measured t(n) = a*n + b*n^2 coefficients mean physically,
  * achieved TFLOP/s against this GPU's matrix peak, and
  * whether the attention split path ever splits a chunk's history.

    dev/prefill_model.py [--gguf PATH] [--rows 2048] [--kv-bits 4]

Not wired into the build. See PREFILL_MODEL.md for the write-up.
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import struct
import sys

# ------------------------------------------------------------------ GGUF read


def _read_str(f):
    (n,) = struct.unpack("<Q", f.read(8))
    return f.read(n).decode("utf-8", "replace")


def _read_value(f, t):
    scalar = {0: "<I", 1: "<i", 2: "<H", 3: "<h", 4: "<I", 5: "<i", 6: "<f", 12: "<d"}
    if t in scalar:
        return struct.unpack(scalar[t], f.read(struct.calcsize(scalar[t])))[0]
    if t == 7:
        return bool(f.read(1)[0])
    if t == 8:
        return _read_str(f)
    if t == 9:
        (et,) = struct.unpack("<I", f.read(4))
        (n,) = struct.unpack("<Q", f.read(8))
        return [_read_value(f, et) for _ in range(n)]
    if t in (10, 11):
        return struct.unpack("<Q" if t == 10 else "<q", f.read(8))[0]
    raise ValueError(f"GGUF metadata type {t}")


def read_gguf_header(path):
    """Metadata and tensor table of a GGUF; stops before the tensor data."""
    with open(path, "rb") as f:
        if f.read(4) != b"GGUF":
            raise ValueError("not a GGUF")
        (version,) = struct.unpack("<I", f.read(4))
        if version != 3:
            raise ValueError(f"unsupported GGUF version {version}")
        (n_tensors,) = struct.unpack("<Q", f.read(8))
        (n_kv,) = struct.unpack("<Q", f.read(8))
        metadata = {}
        for _ in range(n_kv):
            key = _read_str(f)
            (t,) = struct.unpack("<I", f.read(4))
            metadata[key] = _read_value(f, t)
        tensors = {}
        for _ in range(n_tensors):
            name = _read_str(f)
            (nd,) = struct.unpack("<I", f.read(4))
            dims = tuple(struct.unpack("<Q", f.read(8))[0] for _ in range(nd))
            (t,) = struct.unpack("<I", f.read(4))
            (offset,) = struct.unpack("<Q", f.read(8))
            tensors[name] = (t, dims, offset)
    return metadata, tensors


def elements(dims):
    n = 1
    for d in dims:
        n *= d
    return n


# ------------------------------------------------------------------ geometry


class Geometry:
    """The target's shape, from GGUF metadata with the layout constants behind it."""

    def __init__(self, metadata):
        m = metadata
        self.layers = m.get("qwen35.block_count", 64)
        self.hidden = m.get("qwen35.embedding_length", 5120)
        self.intermediate = m.get("qwen35.feed_forward_length", 17408)
        self.vocab = len(m.get("tokenizer.ggml.tokens", [0] * 248320))
        self.q_heads = m.get("qwen35.attention.head_count", 24)
        self.kv_heads = m.get("qwen35.attention.head_count_kv", 4)
        self.head_dim = m.get("qwen35.attention.key_length", 256)
        self.rotary_pairs = m.get("qwen35.rope.dimension_count", 64) // 2
        self.theta = m.get("qwen35.rope.freq_base", 1.0e7)
        self.period = m.get("qwen35.full_attention_interval", 4)
        self.gdn_key_heads = m.get("qwen35.ssm.group_count", 16)
        self.gdn_value_heads = m.get("qwen35.ssm.time_step_rank", 48)
        self.gdn_head_dim = m.get("qwen35.ssm.state_size", 128)
        self.conv_taps = m.get("qwen35.ssm.conv_kernel", 4)
        self.q_width = self.q_heads * self.head_dim
        self.kv_width = self.kv_heads * self.head_dim
        self.value_width = self.gdn_value_heads * self.gdn_head_dim
        # attn_qkv.weight's rows: q|k of 2*key_heads*head_dim, then v.
        self.conv_dim = 2 * self.gdn_key_heads * self.gdn_head_dim + self.value_width
        self.full_layers = [l for l in range(self.layers) if (l + 1) % self.period == 0]
        self.gdn_layers = [l for l in range(self.layers) if l not in set(self.full_layers)]
        self.attention_tile_rows = 8       # SPLASH_PREFILL_ATTENTION_TILE_ROWS
        self.attention_max_splits = 32     # SPLASH_PREFILL_ATTENTION_MAXIMUM_SPLITS
        self.kv_page_tokens = 32           # SPLASH_TARGET_KV_BLOCK_TOKENS


def params_from_tensors(tensors, g):
    """Layer parameters the GGUF actually stores, split by layer kind."""
    full = gdn = 0
    for name, (_, dims, _) in tensors.items():
        if not name.startswith("blk."):
            continue
        n = elements(dims)
        if int(name.split(".")[1]) in set(g.full_layers):
            full += n
        else:
            gdn += n
    return full / len(g.full_layers), gdn / len(g.gdn_layers)


# ------------------------------------------------------------------ work model


def matmul_flops(params_per_token, rows):
    """2 * MACs: every layer weight is touched once per row of the chunk."""
    return 2 * params_per_token * rows


def attention_flops(g, rows, committed):
    """QK^T + PV for `rows` new tokens appended after `committed` tokens.

    One query vector at position p costs 256 MACs for QK^T and 256 for PV, so
    1024 FLOPs per visible token: 4 * q_width * p per layer per token.
    """
    per_layer = sum(4 * g.q_width * (committed + i + 1) for i in range(rows))
    return len(g.full_layers) * per_layer


def attention_tokens_touched(g, rows, committed):
    """KV token-visits: every 8-row query tile walks its whole visible history."""
    tile = g.attention_tile_rows
    tiles = math.ceil(rows / tile)
    return sum(committed + min(rows, (t + 1) * tile) for t in range(tiles))


def kv_bytes(g, tokens_touched, kv_bits):
    """KV bytes read for `tokens_touched` (token, KV head, tensor) visits."""
    per_head = g.head_dim * kv_bits / 8 + 4  # data plus one fp32 scale per head
    return tokens_touched * g.kv_heads * per_head * 2 * len(g.full_layers)


def gdn_flops(g, rows):
    """The GDN recurrence: four 128x128 products per value head per token."""
    d = g.gdn_head_dim
    per_token = g.gdn_value_heads * 4 * 2 * d * d
    per_token += 2 * g.conv_dim * g.conv_taps  # depthwise convolution
    return len(g.gdn_layers) * rows * per_token


def prefill_splits(g, rows):
    """kv::prefillAttentionTiles + ops::prefillSplits, as the code computes them."""
    tiles = math.ceil(rows / g.attention_tile_rows)
    return tiles, min(max(g.attention_max_splits // tiles, 1), g.attention_max_splits)


def dispatches(g):
    """Kernel dispatches one chunk encodes per layer kind.

    From QwenTarget::addPrefill / addPrefillMixer / addPrefillFfn and the ops
    entry points they call. `gguf_rotate` is Linear::addGguf's Hadamard pass,
    which every rotated quantized projection emits before its tiles; the
    prefill tiles fuse the segments of one format into one dispatch
    (Linear::addGgufPrefill), and an F32 segment (GDN alpha/beta) runs
    gguf_float_* first (addGgufFloatSegments).
    """
    ffn = ["norm_rms (post_attention_norm)",
           "gguf_rotate (ffn_gate)",
           "gguf_prefill_pq20_a (ffn_gate)",
           "gguf_rotate (ffn_up)",
           "gguf_prefill_pq20_g (ffn_up, silu(gate))",
           "gguf_rotate (ffn_down)",
           "gguf_prefill_pq20_r (ffn_down, +residual)"]
    gdn = ["norm_rms (attn_norm)",
           "gguf_float_bf16 (ssm alpha|beta, F32 segment)",
           "gguf_rotate (attn_qkv|attn_gate)",
           "gguf_prefill_pq20_a (qkv|gate, one fused dispatch)",
           "prefill_gdn_prepare",
           "prefill_gdn_scan",
           "prefill_gdn_gate",
           "gguf_rotate (ssm_out)",
           "gguf_prefill_pq20_r (ssm_out, +residual)"] + ffn
    full = ["norm_rms (attn_norm)",
            "gguf_rotate (q|k|v)",
            "gguf_prefill_pq20_a (q|k|v, one fused dispatch)",
            "prefill_attention_qkv (q/k norm + RoPE + staging)",
            "prefill_attention_<kv>_store",
            "prefill_attention_<kv>_split",
            "prefill_attention_reduce",
            "prefill_attention_gate",
            "gguf_rotate (attn_output)",
            "gguf_prefill_pq20_r (attn_output, +residual)"] + ffn
    return gdn, full


# ------------------------------------------------------------------ the fit

# The parent's five measured points (int4 KV, greedy, unique filler text).
MEASURED = [(4338, 10.9), (17141, 44.7), (68224, 227.5), (136322, 577.4), (179852, 894.0)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gguf", default=None)
    ap.add_argument("--rows", type=int, default=2048)
    ap.add_argument("--kv-bits", type=float, default=4.0,
                    help="KV format in bits per element (4, 8 or 16)")
    ap.add_argument("--peak", type=float, default=26.5,
                    help="assumed bf16 matrix peak, TFLOP/s")
    ap.add_argument("--alu", type=float, default=6.6,
                    help="assumed fp32 shader-ALU peak, TFLOP/s")
    ap.add_argument("--bandwidth", type=float, default=307.2,
                    help="assumed unified memory bandwidth, GB/s")
    ap.add_argument("--weights-bits", type=float, default=2.125,
                    help="PQ2_0: 34 bytes per 128 elements")
    args = ap.parse_args()

    default = os.path.expanduser(
        "~/development/splash_of_mine/install/models/.resolved/*/target/*PQ2_0.gguf")
    path = args.gguf or next(iter(sorted(glob.glob(default))), None)
    if not path or not os.path.exists(path):
        sys.exit("no PQ2_0 GGUF found; pass --gguf")
    metadata, tensors = read_gguf_header(path)
    g = Geometry(metadata)

    print(f"# {os.path.basename(path)}  {os.path.getsize(path)/2**30:.3f} GiB, "
          f"{len(tensors)} tensors")

    # ---------------------------------------------------------- 1. census
    print("\n## 1. layer census")
    print(f"   layers            {g.layers}")
    print(f"   full attention    {len(g.full_layers)}  {g.full_layers}")
    print(f"   GDN / SSM         {len(g.gdn_layers)}  every layer not listed above")
    print(f"   period            every {g.period}th (isFullAttentionLayer: (l+1) % {g.period} == 0)")
    print(f"   hidden            {g.hidden},  dense FFN {g.intermediate} (QwenFfnKind::Dense)")
    print(f"   attention         {g.q_heads} q heads / {g.kv_heads} kv heads, head dim {g.head_dim},"
          f" q width {g.q_width}")
    print(f"   RoPE              {g.rotary_pairs} pairs ({2*g.rotary_pairs} of {g.head_dim} dims),"
          f" theta {g.theta:g}")
    print(f"   GDN               {g.gdn_key_heads} key / {g.gdn_value_heads} value heads,"
          f" head dim {g.gdn_head_dim}, {g.conv_taps} conv taps over {g.conv_dim} channels")
    full_per_layer, gdn_per_layer = params_from_tensors(tensors, g)
    layer_params = full_per_layer * len(g.full_layers) + gdn_per_layer * len(g.gdn_layers)
    embed = elements(tensors["token_embd.weight"][1]) + elements(tensors["output.weight"][1])
    ffn_params = 3 * g.intermediate * g.hidden
    print(f"   parameters        {full_per_layer/1e9:.4f}B per full layer,"
          f" {gdn_per_layer/1e9:.4f}B per GDN layer")
    print(f"                     {layer_params/1e9:.3f}B in layers + {embed/1e9:.3f}B"
          f" embedding/output = {(layer_params+embed)/1e9:.3f}B")
    print(f"                     FFN is {100*ffn_params*g.layers/layer_params:.1f}% of the layer weights")

    weight_bytes = layer_params * args.weights_bits / 8
    print(f"   weight bytes      {weight_bytes/1e9:.2f} GB of layer weights"
          f" ({args.weights_bits:g} bits/weight)")

    # ---------------------------------------------------------- 2. dispatches
    gdn_d, full_d = dispatches(g)
    per_chunk = len(g.gdn_layers) * len(gdn_d) + len(g.full_layers) * len(full_d)
    print("\n## 2. dispatches per 2048-row chunk")
    print(f"   GDN layer          {len(gdn_d)}")
    for name in gdn_d:
        print(f"      {name}")
    print(f"   full-attn layer    {len(full_d)}")
    for name in full_d:
        print(f"      {name}")
    print(f"   per chunk          {len(g.gdn_layers)}*{len(gdn_d)} + "
          f"{len(g.full_layers)}*{len(full_d)} = {per_chunk}")
    print("   plus              1 embedding gather, 5 capture row copies"
          " (layers 5,19,33,47,61), head norm+logits on the last chunk only")

    # ---------------------------------------------------------- 3. FLOPs
    rows = args.rows
    mm_chunk = matmul_flops(layer_params, rows)
    gdn_chunk = gdn_flops(g, rows)
    intensity = mm_chunk / weight_bytes
    ridge = args.peak * 1e12 / (args.bandwidth * 1e9)
    print(f"\n## 3. work of one {rows}-row chunk")
    print(f"   matmul            2 * {layer_params/1e9:.3f}B * {rows} = {mm_chunk/1e12:.2f} TFLOP"
          f"  (every layer weight; dense, so no expert is skipped)")
    print(f"   attention         4 * {g.q_width} * rows^2/2 * {len(g.full_layers)} layers"
          f" = {attention_flops(g, rows, 0)/1e12:.3f} TFLOP at C=0")
    print(f"   GDN recurrence    {gdn_chunk/1e9:.1f} GFLOP ({100*gdn_chunk/mm_chunk:.2f}% of matmul)")
    print(f"   weight traffic    {weight_bytes/1e9:.2f} GB per chunk,"
          f" {intensity:,.0f} FLOP/byte against a {ridge:.0f} FLOP/byte ridge"
          f" -> {intensity/ridge:.0f}x past it, i.e. compute-bound by construction")
    print(f"   embedding/head    gather of {rows} rows ({rows*g.hidden*2/1e6:.1f} MB);"
          f" the {g.vocab}x{g.hidden} head runs once per prompt, on its last chunk")

    # ---------------------------------------------------------- 4. the fit
    a, b = 2.41147e-3, 13.3943e-9  # s/token, s/token^2 (the parent's fit)
    print("\n## 4. the measured fit, t(n) = a*n + b*n^2")
    print(f"   a = {a*1e3:.5f} ms/token      b = {b*1e9:.4f} ns/token^2")
    print(f"   n        measured   a*n      b*n^2    model     err")
    for n, t in MEASURED:
        lin, quad = a * n, b * n * n
        print(f"   {n:>6}   {t:7.1f} s  {lin:7.1f} s {quad:7.1f} s {lin+quad:7.1f} s"
              f"  {100*((lin+quad)-t)/t:+5.1f}%")
    print(f"   break-even a/b = {a/b:,.0f} tokens;  linear asymptote 1/a = {1/a:.0f} tok/s")

    n = 179852
    t_lin, t_quad = a * n, b * n * n
    print(f"\n   at n={n:,}: linear {t_lin:.1f} s ({100*t_lin/(t_lin+t_quad):.1f}%),"
          f" quadratic {t_quad:.1f} s ({100*t_quad/(t_lin+t_quad):.1f}%)")
    for m in (4338, 68224, 136322, 262144):
        lin, quad = a * m, b * m * m
        print(f"   at n={m:>7,}: linear {100*lin/(lin+quad):4.1f}%  quadratic {100*quad/(lin+quad):4.1f}%")

    # ---------------------------------------------------------- 5. physics
    print("\n## 5. what a and b are, physically")
    per_token_bytes = weight_bytes / rows
    bw_eff = per_token_bytes / a
    r_mm = 2 * layer_params / a
    print(f"   weights: {weight_bytes/1e9:.2f} GB per chunk / {rows} rows ="
          f" {per_token_bytes/1e6:.3f} MB of weights per token")
    print(f"            {per_token_bytes/1e6:.3f} MB / {a*1e3:.3f} ms = {bw_eff/1e9:.2f} GB/s"
          f"  ({100*bw_eff/(args.bandwidth*1e9):.2f}% of {args.bandwidth:.0f} GB/s)")
    print(f"            2 * {layer_params/1e9:.3f}B / {a*1e3:.3f} ms = {r_mm/1e12:.2f} TFLOP/s"
          f"  ({100*r_mm/1e12/args.peak:.0f}% of {args.peak:.1f} TFLOP/s)")
    print(f"            -> the weight path is compute-bound, {intensity/ridge:.0f}x past the ridge;"
          f" DRAM is {100*bw_eff/(args.bandwidth*1e9):.1f}% busy")
    print(f"            sanity: {r_mm/1e12:.1f} TFLOP/s is {r_mm/1e12/args.alu:.1f}x the"
          f" {args.alu:.1f} TFLOP/s fp32 ALU peak, so the tiles do run on the matrix units")

    att_coeff = 2 * g.q_width * len(g.full_layers)   # FLOPs per token^2
    r_att = att_coeff / b
    print(f"   attention: {att_coeff:,} FLOP per token^2"
          f" (= 2 * {g.q_width} * {len(g.full_layers)} layers)")
    print(f"            {att_coeff:,} / {b*1e9:.4f} ns = {r_att/1e12:.2f} TFLOP/s"
          f"  ({100*r_att/1e12/args.peak:.0f}% of {args.peak:.1f} TFLOP/s)")
    print(f"            vs the weight path's {r_mm/1e12:.2f}: attention runs at"
          f" {100*r_att/r_mm:.0f}% of it")
    print(f"            FLOP-bound at peak would be b = {att_coeff/(args.peak*1e12)*1e9:.3f} ns/token^2"
          f" (measured {b*1e9:.3f}, {b/(att_coeff/(args.peak*1e12)):.2f}x)")

    # ---------------------------------------------------------- 6. splits
    print("\n## 6. does the attention split path ever split?")
    print("   rows   tiles  32//tiles  splits   split grid {kvHeads, tiles, splits}")
    for r in (2048, 1676, 1154, 512, 256, 128, 64):
        tiles, splits = prefill_splits(g, r)
        print(f"   {r:>5}  {tiles:>5}   {g.attention_max_splits//tiles:>7}"
              f"  {splits:>7}   {{{g.kv_heads}, {tiles}, {splits}}}"
              f" = {g.kv_heads*tiles*splits} threadgroups")
    print(f"   -> splits == 1 for every chunk of 256 rows or more; only a chunk shorter than"
          f" 256 rows ever splits, and the last chunk of a prompt is 1676 rows at n=179,852")
    print(f"   but the grid already holds {g.kv_heads*256} threadgroups of 256 threads for 16"
          f" cores, so history parallelism is not what is missing")

    # ---------------------------------------------------------- 7. traffic
    chunks = math.ceil(n / rows)
    visits = 0
    for k in range(chunks):
        visits += attention_tokens_touched(g, min(rows, n - k * rows), k * rows)
    print(f"\n## 7. attention traffic at n={n:,}")
    print(f"   KV token-visits   {visits/1e9:.2f} G (one walk per 8-row query tile)")
    for bits in (4, 8, 16):
        r = kv_bytes(g, visits, bits)
        print(f"     {bits:>2}-bit KV: {r/1e9:7.0f} GB, {r/1e9/args.bandwidth:6.1f} s at"
              f" {args.bandwidth:.0f} GB/s = {100*r/1e9/args.bandwidth/t_quad:3.0f}% of the"
              f" {t_quad:.0f} s attention time")

    # Per-threadgroup arithmetic intensity: the 8-row tile against the ridge.
    tile = g.attention_tile_rows
    fused = tile * (g.q_heads // g.kv_heads)
    page = g.kv_page_tokens
    tile_flops = 2 * 2 * fused * g.head_dim * page
    tile_bytes = 2 * page * g.head_dim * (args.kv_bits / 8 + 4 / g.head_dim)
    print(f"\n## 7b. why: the query tile is only {tile} rows")
    print(f"   one threadgroup owns {tile} rows x {g.q_heads//g.kv_heads} q heads per kv head"
          f" = {fused} fused rows of {g.head_dim} dims")
    print(f"   per {page}-token page: {tile_flops/1e6:.2f} MFLOP over {tile_bytes/1e3:.1f} kB"
          f" -> {tile_flops/tile_bytes:.0f} FLOP/byte against a ridge of {ridge:.0f}"
          f" ({tile_flops/tile_bytes/ridge:.1f}x)")
    print(f"   the weight path's tiles are {intensity/ridge:.0f}x past the ridge; these are"
          f" {tile_flops/tile_bytes/ridge:.1f}x, so a DRAM miss stalls a matmul here")
    for t2 in (8, 16, 32, 64):
        f2 = t2 * (g.q_heads // g.kv_heads)
        fl = 2 * 2 * f2 * g.head_dim * page
        print(f"     {t2:>2}-row tile: {fl/tile_bytes:5.0f} FLOP/byte"
              f" ({fl/tile_bytes/ridge:5.1f}x ridge), KV read {kv_bytes(g, visits*tile//t2, args.kv_bits)/1e9:6.0f} GB")

    # Compute/traffic decomposition of the quadratic term.
    att_total = 0
    for k in range(chunks):
        r = min(rows, n - k * rows)
        att_total += attention_flops(g, r, k * rows)
    t_kv = kv_bytes(g, visits, args.kv_bits) / (args.bandwidth * 1e9)
    t_at_weight_rate = att_total / r_mm
    print(f"\n## 7c. splitting the {t_quad:.0f} s attention time")
    print(f"   attention FLOPs   {att_total/1e15:.3f} PFLOP over the whole prompt")
    print(f"   at the weight path's {r_mm/1e12:.2f} TFLOP/s that is {t_at_weight_rate:.0f} s,"
          f" measured {t_quad:.0f} s -> {t_quad/t_at_weight_rate:.2f}x")
    print(f"   KV traffic alone  {t_kv:6.1f} s at {args.bandwidth:.0f} GB/s"
          f" ({100*t_kv/t_quad:.0f}% of the attention time if it were serial)")
    print(f"   -> the shortfall is memory exposure, not arithmetic: the {tile}-row tile reads"
          f" {tile_bytes/1e3:.1f} kB to do {tile_flops/1e6:.2f} MFLOP")
    wider = 32
    wide_flops = 2 * 2 * wider * (g.q_heads // g.kv_heads) * g.head_dim * page
    print(f"   widening the tile to {wider} rows would cut KV reads"
          f" {visits/(visits*tile//wider):.0f}x"
          f" ({kv_bytes(g, visits, args.kv_bits)/1e9:.0f} ->"
          f" {kv_bytes(g, visits*tile//wider, args.kv_bits)/1e9:.0f} GB) and lift intensity"
          f" from {tile_flops/tile_bytes:.0f} to {wide_flops/tile_bytes:.0f} FLOP/byte"
          f" ({wide_flops/tile_bytes/ridge:.0f}x ridge) with no change to the arithmetic")
    print(f"   the additive model: {t_at_weight_rate:.0f} s compute + {t_kv:.0f} s traffic"
          f" = {t_at_weight_rate+t_kv:.0f} s against {t_quad:.0f} s measured"
          f" ({(t_at_weight_rate+t_kv)/t_quad-1:+.1%}) -- compute and KV reads are"
          f" essentially serial, not overlapped")

    # The softmax's thread mapping is what pins the tile at 8 rows.
    lanes_per_row = page // 8
    print(f"\n## 7d. why the tile is 8 rows and not 32")
    print(f"   splash_attention_page_softmax maps one lane per 8 tokens per fused row"
          f" and returns past {lanes_per_row} x fused_rows > 256 threads:")
    for t2 in (8, 10, 12, 16, 32):
        need = lanes_per_row * t2 * (g.q_heads // g.kv_heads)
        print(f"     {t2:>2}-row tile: {need:3d} threads of 256"
              f"  {'ok' if need <= 256 else 'SILENTLY DROPS ROWS'}")
    print(f"   so the tile cannot pass 10 rows without a loop in the softmax;"
          f" the 4x traffic cut needs that loop, not an ABI constant")

    # ---------------------------------------------------------- 8. headroom
    print("\n## 8. headroom at n=179,852")
    print(f"   weights    {t_lin:6.1f} s at {100*r_mm/1e12/args.peak:3.0f}% of peak;"
          f" to peak saves {t_lin*(1-r_mm/1e12/args.peak):5.1f} s"
          f" ({100*t_lin*(1-r_mm/1e12/args.peak)/(t_lin+t_quad):4.1f}% of the prompt)")
    print(f"   attention  {t_quad:6.1f} s at {100*r_att/1e12/args.peak:3.0f}% of peak;"
          f" to peak saves {t_quad*(1-r_att/1e12/args.peak):5.1f} s"
          f" ({100*t_quad*(1-r_att/1e12/args.peak)/(t_lin+t_quad):4.1f}% of the prompt)")
    for label, gain_mm, gain_att in (
            ("weights to 22 TFLOP/s", 22e12 / r_mm, 1.0),
            ("attention to 20 TFLOP/s", 1.0, 20e12 / r_att),
            ("attention to the weight rate", 1.0, r_mm / r_att),
            ("attention to peak", 1.0, args.peak * 1e12 / r_att),
            ("both to peak", args.peak * 1e12 / r_mm, args.peak * 1e12 / r_att)):
        t = t_lin / gain_mm + t_quad / gain_att
        print(f"   {label:<30} {t:6.1f} s  ({100*(1-t/(t_lin+t_quad)):4.1f}% faster)")

    # ---------------------------------------------------------- 9. peak sweep
    print("\n## 9. how the answer moves with the assumed peak")
    print("   16 cores x 1024 FLOP/cycle (fp16/bf16 matrix) x 1.62 GHz = 26.5 TFLOP/s theoretical;")
    print("   the 20.2 TFLOP/s the weight path already achieves proves the units deliver at")
    print("   least that, so the peak is not below 20.2.")
    for peak in (20.2, 26.5, 28.7):
        print(f"   peak {peak:4.1f} TFLOP/s: weights {100*r_mm/1e12/peak:4.0f}%,"
              f" attention {100*r_att/1e12/peak:4.0f}% of peak;"
              f" both to peak -> {t_lin*r_mm/1e12/peak + t_quad*r_att/1e12/peak:6.1f} s"
              f" ({100*(1-(t_lin*r_mm/1e12/peak + t_quad*r_att/1e12/peak)/(t_lin+t_quad)):4.1f}% faster)")


if __name__ == "__main__":
    sys.exit(main())
