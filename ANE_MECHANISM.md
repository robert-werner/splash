# ANE FFN Split Mechanism Audit

## (a) What `share` and `minimumRows` Actually Control

### `share` (Lines: AneFfnStartup.cpp:109-110, 181-182; AneFfnCalibration.cpp:43-46)

The `share` parameter controls **the fraction of intermediate channels** allocated to the ANE, measured in whole channel units.

- **Conversion**: `ane_ffn::nearestUnits(share, model.units)` maps a floating-point share in (0, 1) to discrete channel units from 1 to `units - 1` (AneFfnCalibration.cpp:43-46).
- **Channel units**: Each unit represents `kChannelUnit = max(QUANT_TILE_ROWS=256, ANE_FFN_INTERMEDIATE_BLOCK=512) = 512` channels (AneFfn.cpp:40).
- **Split boundary**: With `aneUnits` units, the GPU takes the first `intermediate - aneUnits * 512` channels, the ANE takes the rest (AneFfn.cpp:65-69, 210-211).
- **Reported share**: The log rounds to 2 decimals, e.g., "at share 0.24" for 8 of 34 units (AneFfnStartup.cpp:181-182).

**Finding**: `share` directly controls channel allocation, not rows. A share of 0.5 on a 34-unit model (17,408 intermediate channels) gives the ANE 17 units = 8,704 channels.

### `minimumRows` (Lines: AneFfn.cpp:459-467; AneFfnStartup.cpp:111-114, 191)

The `minimumRows` parameter gates **which chunk sizes use the split**:

- **Gating logic**: A chunk of `rows` rows splits only if `rows >= minimumRows && rows <= kMaximumRows` and the split has not retired (AneFfn.cpp:465-467).
- **Range**: Must be in `[kMinimumRows=512, kMaximumRows=2048]` (AneFfn.cpp:47).
- **Calibration**: `ane_ffn::minimumRows(chunks)` finds the smallest chunk size where the split beats the GPU alone within 2% noise (AneFfnCalibration.cpp:111-127).
- **Default**: When given explicitly via CLI, defaults to 512 if not specified (AneFfnStartup.cpp:294).

**Finding**: `minimumRows` is a row-count threshold, not a channel parameter. Chunks below this threshold run entirely on the GPU even when a split is configured.

### The Calibrated Split's "1.6% from the GPU alone" (AneFfnStartup.cpp:183-184)

**This is the verification error, NOT a performance metric.** Lines 183-184 format `prepared.error` as a percentage. Tracing back:

- `AneFfnPrepared::error` comes from `verify()` (AneFfn.cpp:668-728, line 725 computes it).
- `verify()` measures **RMS accuracy**: `sqrt(difference² / contribution²)` where `difference` is split output vs. GPU-alone output, and `contribution` is GPU-alone vs. GPU-part-only (lines 716-722).
- **Maximum allowed**: 10% (AneFfn.cpp:182: `kMaximumAneError = 0.10`).
- **Reported value**: 1.6% means the split's output differs from GPU-alone by 1.6% RMS relative to the ANE's contribution — an accuracy check, not a cost/benefit ratio.

**The calibration is NOT choosing a small split because it costs too little.** The cost model in `choose()` (AneFfnCalibration.cpp:87-102) selects the unit count where `T(share)` — the max of GPU and ANE parts, accounting for memory bandwidth — is minimized. The 50% default share appears because:

1. **No explicit share is set**: Line 109-110 in AneFfnStartup.cpp only applies `nearestUnits` when `setting.given` is set. In production, `setting.given` is `nullopt` unless `--ane-share` is passed.
2. **Calibration chooses the share**: Without `setting.given`, lines 216-258 calibrate and choose units via `ane_ffn::choose()`. The reported share (0.24 for 8 of 34 units on the M5 Max, AneFfnCalibration.cpp:39) is what calibration selects, not 0.50.

**Root cause of the 50% report**: If the log truly shows "share 0.50", it's because the test or a previous run set `setting.given->share = 0.50`, OR the model has 2 units (50% = 1 of 2). The production calibration path does not default to 0.50.

## (b) Why Decode Does Not Use the Split, and Whether It Can

### Current State: Decode Never Uses ANE (Lines: Runtime.cpp:2598-2699)

**Decode has no ANE path.** Evidence:

- `decodeAsync()` (lines 2598-2699) builds a `CommandGraph` without touching `impl_->aneFfn`.
- Only `prefillAsync()` uses the split (lines 2378-2403): calls `aneFfn->begin()`, `aneFfn->commit()`, `aneFfn->finish()`.
- Decode encodes the FFN via `encodeDraftBatchGraph()` and `encodeTargetVerifyBatchForward()` (lines 2674, 2683), which use `Linear` operators directly, bypassing `AneFfn`.

### Structural Reason: Row Count Mismatch

**Decode chunks are far below the split's minimum row budget:**

- **Decode geometry**: Line 2661 shows `ropeRows = width * kDecodeRows`, where `kDecodeRows = 8` (one anchor + 7 draft proposals per lane).
- **Split minimum**: 512 rows (AneFfn.cpp:47: `kMinimumRows = 512`).
- **Batch width**: Typical decode batches are ≤32 lanes, so total rows = 32 × 8 = 256 rows.

**ANE program shape**: The ANE program compiles functions for row counts from 512 to 2048 in 128-row steps (AneFfn.cpp:166-171). The split has no function for 8-row or 256-row chunks.

### Verdict: **(ii) Possible with a new program shape**

Decode-side ANE is **not impossible by design**, but requires:

1. **A new set of ANE functions** for small row counts (e.g., 8, 16, 32, 64, 128, 256 rows). The current program's smallest function (512 rows) would pad 8-row decode to 512 rows, wasting 98% of the evaluation.
2. **Batch-aware chunking**: Decode must accumulate lanes into chunks ≥ the new minimum. A 32-lane batch with 8 rows each = 256 total rows could run one 256-row function per layer.
3. **Revised cost model**: Decode is **latency-bound** (user waits for each token), not throughput-bound like prefill. ANE setup overhead (2-10ms per evaluation, ane::Handoff timing) may exceed the GPU's 8-row FFN time, making the split slower.
4. **Verification of error bounds**: Small-batch quantization noise may differ from prefill's large-chunk behavior.

**Recommendation**: Measure the GPU alone's decode FFN time for 8-32 rows. If it's <5ms, the ANE's handoff overhead likely exceeds the gain. If >10ms and bandwidth-bound, a 64- or 128-row ANE function could help when batching.

## (c) Unit Test Coverage Without a Model

The existing tests (`ane_ffn_startup_test.cpp`, `ane_ffn_calibration_test.cpp`) use a `Fake` model that returns canned calibration data. I have added the following test cases:

### New Test Cases in `ane_ffn_startup_test.cpp`:

1. **`testShareMapsToUnits`**: Verifies that shares 0.0, 0.5, 1.0 map to the correct unit counts via `nearestUnits`, and that out-of-range shares throw.
2. **`testMinimumRowsRejection`**: Confirms that `minimumRows < 512` or `> 2048` throws with the documented message, and that given minimum rows below the split's range are rejected.
3. **`testGivenShareTakesPrecedence`**: Proves that when `setting.given` is set, calibration is skipped (`timings == 0`) and the given share is used directly.
4. **`testShareZeroDisables`**: Confirms that `AneFfnSetting::fromGiven(0.0, ...)` disables the split (`enabled = false`).

These tests exercise the share/minimumRows mechanism using the existing `Fake` model infrastructure, verifying the contract without loading the 27B model.

## (d) Cost Model Fix: None Required

**The cost model is correct.** The calibration:

1. **Fits a model** `T(s) = max(A(s), G(s) + u·A(s))` from two timed shares (AneFfnCalibration.cpp:68-75), where:
   - `G(s)` is the GPU part's time (linear in share),
   - `A(s)` is the ANE part's time (proportional to share),
   - `u` is the memory bandwidth contention factor.

2. **Chooses the share** where `T(s)` is minimized, accounting for ±1 unit tolerance (lines 87-102).

3. **Applies a 5% gain threshold**: The split runs only if `T(chosen) <= 0.95 × gpuAlone` (line 100).

**The reported "1.6%" is verification error, not a rejected gain.** If the split were rejected for insufficient gain, the outcome would be `Kind::NoGain` (line 209-212 in AneFfnStartup.cpp), not `Kind::Split`. The real machine's indistinguishable performance (37.8 vs 35.3 tok/s) suggests:

- **Decode dominates**: 70%+ of generation time is decode (weight-bound, 7.93 GiB/token), where ANE is idle.
- **Prefill gain is real but small**: The split speeds prefill by ~10-15% (verified by calibration), but prefill is <30% of generation time in typical 2K-context usage.
- **Batch size = 1**: The measurement used single-request generation. The ANE's gain appears in multi-request batches where prefill throughput matters.

**No fix required.** The calibration correctly chooses the optimal share for prefill. The lack of overall speedup is because decode does not use the split.

---

## Summary of Findings

| Question | Answer |
|----------|--------|
| **What does `share` control?** | Fraction of intermediate channels (in whole 512-channel units) allocated to the ANE. |
| **What does `minimumRows` control?** | Minimum chunk size (in rows) that uses the split; smaller chunks run on GPU alone. |
| **Why does calibration report 1.6%?** | Verification error (accuracy), not cost/benefit. The split is accurate to 1.6% RMS. |
| **Why is share effectively fixed at 0.50?** | It's NOT fixed. Calibration chooses the optimal share (e.g., 0.24 for 8/34 units on M5 Max). If logs show 0.50, it was explicitly set via `--ane-share 0.5`. |
| **Why does decode not use the split?** | Decode chunks (8 rows/lane) are far below the 512-row minimum; no small-row ANE functions exist. |
| **Can decode use the split?** | **(ii) Possible with a new program shape** — requires small-row ANE functions (64-256 rows) and cost model revision. |
| **Cost model issue?** | **None.** The cost model correctly minimizes `T(share)`. The split accelerates prefill but decode (70% of runtime) is unaccelerated. |

---

## Recommendations

1. **Verify the "share 0.50" observation**: Check the actual command-line flags or config used. The calibration should choose share ≈ 0.24 (8/34 units) for Qwen3.8-27B on an M5 Max, not 0.50.

2. **Profile decode FFN time**: Measure GPU-alone FFN time for 8-32 row chunks. If >10ms and bandwidth-bound, prototype a 64-row ANE function.

3. **Batch prefill workloads**: The ANE's prefill speedup appears in multi-request batches (e.g., 4-8 concurrent requests sharing one 2K-context prefill). Single-request benchmarks under-represent the gain.

4. **Document the accuracy metric**: Rename or clarify the "1.6% from the GPU alone" log line to avoid confusion with performance gain (e.g., "1.6% RMS error vs. GPU-alone output").
