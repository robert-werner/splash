# Archive

Superseded phase, week and completion reports, kept for history only.

**Start at [`../../STATUS.md`](../../STATUS.md).** It is the authoritative
statement of what the engine does, what was measured, and what is settled. Where
a file here disagrees with it, STATUS.md is right.

Every file in this directory carries a header naming what in it is now known to
be wrong:

```
> SUPERSEDED by STATUS.md — <the specific retracted claim>
```

A handful say *nothing material is wrong* — those are notes whose findings were
absorbed into STATUS.md (`PHASE2_RESULTS_PTQ1.md`, `PTQ1_0_LAYOUT.md`,
`FLAG_AUDIT.md`, `PRIORITY_SCHEDULING.md`, `PHASE3_PHASE4_SUMMARY.md`). They
were moved so that the repository root holds exactly one status document.

## The recurring errors

Most of the retractions fall into five groups:

1. **ANE hybrid quantization** promised +30% decode and 35–40% ANE utilisation,
   and several files call it 95% complete or production ready. The ANE split
   needs unrotated weights; both Prism formats are rotated and no unrotated
   quantized variant of the checkpoint exists. It is impossible, not pending.
2. **PTQ1_0** described as 18 bytes per block, or as a 1-bit binary format. It
   is 28 bytes of ternary base-3 codewords, and it decodes bit-identically to
   PQ2_0.
3. **WeightPrefetch** credited with 8–10% decode latency. It walks resident
   Metal buffers; weights are read with `pread` + `F_NOCACHE` and there is no
   `mmap` in `runtime/`, so there is nothing to fault in.
4. **AsyncKvSwap** credited with extending context to 1–2M tokens. A swap-out
   cannot return a live request's RAM to the pool, so it cannot extend context.
5. **Baselines of 62.9 or 107 tok/s** quoted without their launch path. They are
   not comparable to the reference 39.4 tok/s measurement.

These are collected with their verdicts in STATUS.md §6, "Closed questions".
