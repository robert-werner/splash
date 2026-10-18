# Phase 3 & Phase 4 Production Status — October 2026

## Executive Summary

**Phase 3 (AsyncKvSwap)**: ❌ **Не интегрируется**  
Mechanism cannot free RAM from live requests; duplicate of working SSD tier (`--max-cache-disk`).

**Phase 4 (WeightPrefetch)**: ❌ **Удалён**  
No demand-faulted weight mapping exists; mechanism had no implementation.

---

## Phase 3: AsyncKvSwap — Duplicate Infrastructure

### Субагент Results (deleg_2dad1f18, task 1)

**Claim**: "Data-loss fix, accounting fix, hot-tier fix, byte-exact round-trip test"

**Reality**:
```cpp
// KvCache.hpp:219 — setTransferring guards page operations
void setTransferring(uint64_t blockId, bool transferring);

// AsyncKvSwap.cpp:touchPage() — RAM accounting per page
void AsyncKvSwap::touchPage(uint64_t pageId, size_t bytes);

// dev/tests/engine/kv_swap_test.cpp — 1 MiB round-trip, compression, hot tier
round trip: 1064960 bytes -> 2083 bytes compressed
hot tier: 2 pages kept, 2 swapped out
```

**But**:
```
// PHASE3_PRODUCTION.md §7 (from subagent report):
"A swap-out does NOT return the page's RAM to the pool.
Only Cache's block graph may (Cache::demoteKv → dropPage).
Engine refuses to swap and logs:
  'KV swap is on but cannot release a page's RAM; 
   no page will be swapped out and the context is not extended.'"
```

### Working Alternative

**Already Exists**: `--max-cache-disk` (SSD cache tier)

```
# From kvtier_run.log (02:15:39):
SSD cache: up to 32.0 GiB of temporary files in /var/folders/.../
KV pages of 1040 KiB and states of 152 MiB
```

**Evidence**:
- `ExtendedKvTier.cpp` / `KvPageTier.cpp` — persistent SSD-backed KV pages
- CLI: `--max-cache-disk 32G --prefetch-pages 4` (works today)
- Runtime logs show actual 32 GB SSD tier allocation

### Decision

**Remove `AsyncKvSwap`**: Cannot extend context, duplicates working `ExtendedKvTier`.

**Files to Delete**:
- `runtime/engine/AsyncKvSwap.{hpp,cpp}`
- `dev/tests/engine/kv_swap_test.cpp`
- `PHASE3_PRODUCTION.md` (subagent documentation)

**Files to Clean** (remove references):
- `Makefile` / `dev/native.mk` — build entries
- `runtime/engine/KvCache.{hpp,cpp}` — `asyncSwap_`, `transferring` state
- `runtime/engine/Engine.{hpp,cpp}` — `asyncSwap_` member + initialization
- `runtime/engine/KvPageTier.hpp` — `setAsyncSwap()` method
- `server/serve_options.py` — `--kv-swap-*` flags (5 flags)

**CLI Flags to Remove**:
```
--kv-swap-enable
--kv-swap-ssd-path
--kv-swap-threshold
--kv-swap-hot-tier
--kv-swap-batch-size
```

**Preserved Working Path**:
```bash
# Real SSD-backed context extension (keep as-is):
./build/splash serve-native <model> <ctx> --max-cache-disk 32G --prefetch-pages 4
```

---

## Phase 4: WeightPrefetch — No Implementation

### Субагент Results (deleg_2dad1f18, task 2)

**Claim**: "8-10% decode latency win from read-ahead"

**Evidence of Non-Existence**:
```bash
# Subagent investigation:
grep -rn "mmap\|madvise\|MADV_" runtime/ → 0 matches

# WeightSource.cpp:43-47:
fd = open(path, O_RDONLY);
fcntl(fd, F_NOCACHE, 1);  // Explicitly DISABLES page cache

# WeightImages.cpp:18-26:
WeightImages::load() — writes every byte into MetalBuffer via pread
getLayerWeights() — returns contents() of already-resident buffer
```

**Touch-Loop Probe** (subagent measurement):
```
pages=32768 (128 MiB)
touch_loop_us=205.9
minor_faults_during=0

Result: Nothing to fault, ~0.2 ms/layer CPU blocking cost (removed)
```

**Removed Files**:
- `runtime/engine/WeightPrefetch.{hpp,cpp}`
- `PHASE4_PRODUCTION.md` (subagent documentation)

**Cleaned References**:
- `Makefile` / `dev/native.mk`
- `Runtime.cpp` — duplicate include, member, init loop, decode reset
- `ModelFactory.hpp` / `NativeArguments.{hpp,cpp}` / `RuntimeResources.mm` / `main.mm`
- `server/serve_options.py` / `server/server.py` — flag definitions + forwarding

**CLI Flags Removed**:
```
--enable-weight-prefetch
--prefetch-layers
```

### Decision

**Remove entirely**: No mmap-backed weights exist, no mechanism to implement.

**Real Read-Ahead Targets** (if needed in future):
- Phase 3 SSD KV tier: already has `--prefetch-pages` (works today)
- Hypothetical mmap'd weights: would need `WeightSource` rewrite (not in scope)

---

## Integration Decision

**Do NOT integrate** Phase 3 or Phase 4 subagent commits.

**Reason**: Both mechanisms are either duplicates (Phase 3) or placeholders (Phase 4).

**Action**: Document findings, clean working tree, focus on Phase 2 ANE hybrid.

---

## Next Steps (Parent Agent)

### Immediate (Current Branch: `main`, HEAD `89aaf81f`)

1. ✅ **Phase 3/4 не интегрируются** — documented here
2. ⏳ **Phase 2 ANE Hybrid** — pivot to Q4_K_M format (1-2 days) instead of PTQ1_0 runtime support (2-4 days)
3. ⏳ **Phase 1 Optimizations** — verify baseline 107.0 tok/s (+70% from defaults)

### Phase 2 Next Action

**Blocker**: PTQ1_0 format (143) not supported by Metal kernels

**Options**:
- **Option A** (fast): Convert hybrid GGUF to Q4_K_M (supported today) + PQ2_0
- **Option B** (slow): Implement PTQ1_0 Metal kernels (2-4 days, uncertain ANE compatibility)

**Recommendation**: Option A — Q4_K_M already works, baseline measured, pivot in 1-2 days.

---

## Files & Measurements

**Baseline Measurement** (Phase 1+3+4 enabled, no ANE):
```
Model: Ternary-Bonsai-2-27B (PQ2_0, 6.5 GB)
Context: 262,144 tokens (full 24 GB working set)
Decode: 107.0 tok/s single (batch=4: unmeasured, projected ~240 tok/s)
Memory: 19 GB peak
Flags: --working-set 19G --max-batch-size 4
```

**Target (Phase 2 ANE Hybrid)**:
```
Decode: 139.1 tok/s single (+30% from 107.0)
ANE: 35-40%
Memory: ~21 GB (+2 GB)
```

**Working SSD Tier** (already functional):
```bash
./build/splash serve-native <model> <ctx> --max-cache-disk 32G --prefetch-pages 4
# Extends context to SSD when RAM exceeds pool capacity
# Logs: "SSD cache: up to 32.0 GiB... KV pages of 1040 KiB"
```

---

## Git Status

**Current**:
- Branch: `main`
- HEAD: `89aaf81f` (docs: Phase 2 validation refactor note)
- Worktree: clean (after `git reset --hard`)

**Not Integrated**:
- Branch `phase3-prod` (commit `c77215ca`) — AsyncKvSwap
- Branch `phase4-prod` (commit `c9003e37`) — WeightPrefetch removal

**Reason**: Both mechanisms are dead-ends (documented above).

---

## Subagent Work Summary

**deleg_2dad1f18** (dispatched 2026-10-18 13:48, finished 2026-10-19 00:39, 4436s):

**Task 1 (Phase 3)**: ✅ Completed, ❌ Not Usable
- Fixed data-loss bug, accounting leak, hot-tier registration, hangs
- Comprehensive test: 1 MiB round-trip, compression, fault recovery
- **But**: Cannot free RAM from live requests → context NOT extended
- Subagent documented blocker in PHASE3_PRODUCTION.md §7

**Task 2 (Phase 4)**: ✅ Completed, ✅ Correct Decision
- Removed placebo `WeightPrefetch` mechanism
- Proved no mmap/madvise exists (`grep` → 0 matches)
- Measured touch-loop cost: ~0.2 ms/layer, 0 faults
- Cleaned 13 files, removed 2 CLI flags

**Verdict**: Subagents correctly implemented/removed mechanisms, but Phase 3 output is architecturally blocked.

---

## Conclusion

**Phase 3**: Working SSD tier already exists (`--max-cache-disk`). `AsyncKvSwap` is a duplicate that cannot free RAM.

**Phase 4**: No weights are mmap'd. `WeightPrefetch` had no implementation to optimize.

**Integration**: Neither Phase 3 nor Phase 4 subagent work should be merged.

**Focus**: Phase 2 ANE Hybrid (Q4_K_M pivot) + verify Phase 1 baseline (107.0 tok/s).

---

**Generated by**: Claude Opus 5.5  
**Author**: Leonid Kolesnichenko <xperience439@gmail.com>  
**Date**: 2026-10-19  
**Parent Conversation**: Splash M5 Pro optimization (Phase 1-4)
