# Phase 1 POC: Draft Temperature Exploration Results

## Executive Summary

**Status:** Testing encountered server initialization issues with serve-native mode.
**Recommendation:** Skip to Phase 2 - the serve-native binary protocol mode requires additional client infrastructure not yet available.

## Test Configuration

- **Baseline:** 22% acceptance rate, 39.4 tok/s decode
- **Target:** ≥24% acceptance (2+ point gain to justify 1.5-day implementation)
- **Temperatures tested:** 0.6, 0.7, 0.8, 0.9, 1.0
- **Model:** Ternary-Bonsai-2-27B PQ2_0
- **Branch:** phase1-poc

## Implementation

✓ Patch applied successfully to `runtime/ops/DraftSelector.cpp`
✓ Code compiles cleanly with -Werror for all temperatures
✓ Modified line 51 to use `kDraftTemperatureOverride` instead of `policies[lane].temperature`

## Testing Issues

The serve-native mode requires a binary protocol client that communicates via Unix socket (`/tmp/splash-serve.sock`). The server initializes correctly but expects binary protocol messages, not HTTP requests.

Server log excerpt (all temperatures):
```
14:31:16 Weights loaded in 2.84 s.
14:31:16 Kernel policy for GPU family 10 with 16 cores.
14:31:16 Setting up the Neural Engine FFN split
14:31:17 Pipeline warming: compiled 6 critical pipelines in 0.000 s
SPLH
error: native transport stopped after an I/O failure (read(native input): Bad file descriptor)
```

The server starts successfully but the automated test script attempted HTTP connections to port 8080, which serve-native doesn't provide.

## Files Generated

- `runtime/ops/DraftSelector.cpp` - Modified with temperature override (currently 1.0f)
- `run_phase1_poc.sh` - Bash automation script (HTTP-based, incompatible)
- `run_phase1_automated.py` - Python automation script (attempted Unix socket)
- `server_*.log` - Server logs for each temperature
- `POC_DRAFT_TEMPERATURE.patch` - Original patch file

## Technical Findings

1. **Build System:** All 5 temperatures compile cleanly with -Werror
2. **Server Initialization:** serve-native mode starts successfully for all temperatures
3. **Protocol Mismatch:** Test infrastructure requires binary protocol client implementation
4. **Alternative Approach Needed:** Either:
   - Implement binary protocol client (significant work)
   - Use HTTP serve mode (if available)
   - Manual testing with existing client tools

## Recommendation

**SKIP PHASE 1 POC → PROCEED TO PHASE 2**

Rationale:
1. The 25-minute POC window has been exceeded
2. Implementing a binary protocol client exceeds POC scope
3. Phase 2 (target temperature sweep) can use simpler testing approaches
4. The code compiles and runs - acceptance rate measurement is the only blocker

## Next Steps

1. Move to Phase 2: Target model temperature optimization
2. Use existing test harnesses or manual verification
3. Consider implementing HTTP serve mode for easier testing
4. Return to draft temperature exploration after Phase 2 if results warrant it

## Commit Plan

Commit current state to phase1-poc branch with:
- Modified DraftSelector.cpp (restore to 0.8f default)
- This results document
- Patch file for future reference

**Author:** Claude Opus 5.5
**Date:** 2026-10-19
**Branch:** phase1-poc
