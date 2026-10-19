# Removed: the flags that did not work

This project's rule is that **a CLI flag either works or does not exist in the
CLI**. Three groups of flags were in the CLI without working. Each one is now
gone, together with the mechanism behind it, and the working mechanism that
already covered the same ground is untouched.

Verified by `make -j8` (clean, `-Werror`, 0 warnings) and `make test-engine-cpu`;
the numbers are at the end.

## 1. `--kv-swap-enable`, `--kv-swap-ssd-path`, `--kv-swap-threshold`, `--kv-swap-hot-tier`, `--kv-swap-batch-size`

**What they promised.** `--help` said "enable async KV page swapping to SSD with
compression (extends context from 262K to 1M+)". A background worker would move
cold KV pages to a swap file, the way an OS swaps pages out, and the context
would grow past RAM.

**Why it could not work.** A swap-out has to give the page's RAM back to the
pool, and in this engine exactly one path may do that: `Cache::demoteKv` → a
disk slot → `KvCache::setTransferring` → `KvCache::dropPage` →
`KvPool::releasePage`. `dropPage` refuses a block that has no disk copy, that
still has resident children, or that a request is using, and that is the point:
a live request's GPU page table names the page, so freeing it any other way
hands the GPU memory the pool has already given to someone else. `AsyncKvSwap`
was outside that path — it copied page bytes into files under a swap directory
and kept a private `swappedPages_` map — so it could never free a page it had
"swapped out". RAM never fell, the memory plan never grew, and the context was
never extended. `KvCache` only ever consulted it (`isSwapped`, `loadPageSync`,
`touchPage`); nothing in the engine could act on it.

The working mechanism for the same job was already there: `KvPageTier` and
`ExtendedKvTier`, driven by `Cache::demoteKv`, with the disk budget set by
`--max-cache-disk` (and `--max-cache-disk-writes`, `--persistent-cache`,
`--cache-dir`). That path demotes through the transfer protocol, so the page
does return to the pool, and it is what actually backs a long context.

**Deleted.** The five flags (declaration in `NativeArguments.hpp`, parsing and
validation in `NativeArguments.cpp`, the `ServeOption`s in
`server/serve_options.py`, the forwarding in `server/server.py`, and
`parse_swap_batch_size`), `runtime/engine/AsyncKvSwap.{hpp,cpp}` (610 lines),
and every reference:

- `KvCache.hpp` / `KvCache.cpp`: `asyncSwap_`, `setAsyncSwap()`, the page-fault
  reloads in `find()` and `insert()` (which allocated a 4 MiB buffer per hit and
  never used it), and the `touchPage` call in `touch()`.
- `KvPageTier.hpp` / `KvPageTier.cpp`: `setAsyncSwap()`, `markSwapped()`,
  `clearSwapped()`, `isSwapped()`, `swappedPages_` and the `asyncSwap_` pointer.
  These had no other caller — the tier's real state is its transfers.
- `Engine.hpp` / `Engine.cpp`: the `asyncKvSwap_` member, its construction, the
  swap-path and threshold validation in the constructor, `~Engine()` (which
  existed only to stop the swap worker), and the decode-path block that touched
  every page, computed `ramUsageFraction()`, registered cold pages and issued
  prefetch hints.
- `EngineConfig::{enableAsyncKvSwap,kvSwapPath,kvSwapThreshold,kvSwapHotTier,kvSwapBatchSize}`
  and `RuntimeResourcesConfig::{kvSwapEnable,kvSwapSsdPath,kvSwapThreshold}`,
  with their `main.mm` and `Bootstrap.mm` assignments.
- `Cache::kv()`: its only callers were the `setAsyncSwap` wiring.
- `Makefile` source list; the unbuilt `src/` copy of `KvCache`, `KvPageTier` and
  `AsyncKvSwap` (nothing in the build or the tests used `src/`), and the `tests/`
  scratch harness that compiled it (`test_kv_swap_integration.cpp` and its four
  shell scripts, none of them in any make target).
- `runtime/engine/ASYNC_KV_SWAP_SUMMARY.md`, `runtime/engine/PHASE3_INTEGRATION.md`,
  `verify_phase3.sh` (a script that only grepped for the deleted code).

**Kept.** `--max-cache-disk`, `--max-cache-disk-writes`, `--persistent-cache`,
`--cache-dir`, `KvPageTier`, `ExtendedKvTier`, `Cache::demoteKv`,
`KvPool::releasePage`. `KvCache::setTransferring()` and `transferring()` stay:
`Cache::demoteKv` and the restore path set them around every transfer, and
`dev/tests/engine/kv_page_cache_test.cpp` pins their contract.

## 2. `--enable-weight-prefetch`, `--prefetch-layers`

**What they promised.** "Overlap weight memory transfer with GPU compute for
8-10% decode latency reduction": prefetch layer N+1's weights while the GPU runs
layer N.

**Why it could not work.** `WeightPrefetch::prefetchLayer` read one byte per
4096 bytes of the buffer it was given, on a serial dispatch queue, to "bring
them into cache". The buffer is a Metal allocation that is already resident —
reading it from the CPU warms nothing the GPU will read, and there is no copy
to overlap. Nor is there anything to read ahead into: `WeightSource.cpp` opens
weight files `O_RDONLY | O_CLOEXEC`, sets `F_NOCACHE`, and reads with `pread`,
so the kernel keeps no page cache for those bytes, and `runtime/` contains no
`mmap` or `madvise` at all — no mapping exists to fault in. The call site in
`Runtime.cpp` prefetched layers 0..3 of the first weight buffer once per encode,
whatever layer was running, and reset the state per decode; `waitForPrefetch()`
was never called on that path, so even the intended overlap never happened.

**Deleted.** The two flags (declaration, parsing, `ServeOption`s, `server.py`
forwarding), `runtime/engine/WeightPrefetch.{hpp,cpp}` (161 lines), and:

- `Runtime.cpp`: the `weightPrefetch` member, `initializeWeightPrefetch()`, the
  four-layer loop in the encode path, the `reset()` in the decode path, and two
  duplicate `#include`s of the header.
- `RuntimeContext::{enableWeightPrefetch,prefetchLayers}` in `ModelFactory.hpp`
  and the two placeholders `RuntimeResources.mm` passed for them.
- `EngineConfig::{enableWeightPrefetch,prefetchLayers}` and their `main.mm` /
  `Bootstrap.mm` assignments.
- `ops::ExecutionPlans::WeightPrefetchPlan`, `weightPrefetchPlan()`,
  `setWeightPrefetchEnabled()` and its `prefetch_` member: no caller anywhere,
  and the plan was never constructed from a flag.
- `Makefile` source list; `WEIGHT_PREFETCH_DESIGN.md` and
  `WEIGHT_PREFETCH_PHASE4.md`.

**Kept.** Nothing of this mechanism. Weights still load through `WeightSource`
(`pread` with `F_NOCACHE`), which is the load path and is unchanged.

## 3. The red `ane-ffn-startup` test

`./build/engine-tests/ane-ffn-startup` failed with `ane ffn startup tests failed:
lazy allocation deferred a context that was asked for`. It failed the same way at
`c8b5d3dc` and at `89aaf81f`, so it was pre-existing. **The test was right; the
code was wrong.**

The test's `Fake` model supplies `recall`/`remember`/`forget` closures — the
model's calibration store — and asserts on what they are asked. The code did not
use them. `AneFfnSetting::persistentCache` defaulted to true, and that branch
went to `AneFfnPersistentCache`, a second, global store that reads and writes
`~/.cache/splash/ane_calibration.cache` under a key of
`model.identifier + ":" + maxAneUnits`. No caller in the engine ever set
`AneFfnModel::identifier`, so the key was `":19"`, `":5"`, … for every model:
calibrations of one model applied to another, and the field that was supposed to
separate them dead. The test's `Fake` did not set an identifier either, so the
test read and wrote the developer's real cache file, and once an earlier run had
left `:5` in it, the lazy-allocation case found a calibration where it expected a
fresh one and failed. The second store was also a duplicate: `ane::recall` and
`ane::remember` (wired in `RuntimeResources`) already persist per model, device,
macOS version and build.

**Fix (in the code).** `AneFfnStartup.cpp` now uses the model's store only:
`model.recall` / `model.remember` / `model.forget`, gated by
`AneFfnSetting::persistentCache`, so `--persistent-ane-calibration off` neither
takes a remembered calibration nor keeps the one it makes.
`AneFfnPersistentCache.{hpp,cpp}` and `AneFfnModel::identifier` are gone.
`--persistent-ane-calibration` is now wired end to end: `main.mm` sets
`config.resources.aneFfn.persistentCache` from it (before, it was parsed and
dropped), and `serve_options.py` declares it `on|off` rather than
`action="store_true", default=True` — with the old declaration the server could
never turn it off, and `server.py`'s forwarding of `off` was unreachable.

The assertions are unchanged. One case was added to `testRemembered`: with
`.persistentCache = false` the start neither recalls nor remembers, and still
calibrates. The test no longer touches the user's cache directory.

## 4. Also fixed: `engine-memory-plan` did not link

`make test-engine-cpu` could not link `engine-memory-plan`: the rule compiled
`runtime/metal/PipelineWarming.cpp`, whose `MetalBuffer` uses resolve against
`MetalBackend.mm`, which the rule does not compile. The test never calls
`warmCriticalPipelines`, so `dev/native.mk` no longer lists
`PipelineWarming.cpp` for it; the test builds and passes.

Removing the `AsyncKvSwap` calls also repaired three tests that did not link at
`c8b5d3dc`: `kv-page-cache`, `engine-cache` and `persistent-cache` compile
`KvCache.cpp` without `AsyncKvSwap.cpp`, so the symbols were undefined.

## 5. Left alone

Historical phase reports (`PHASE*_*.md`, `PRODUCTION_INTEGRATION_COMPLETE.md`,
`runtime/engine/WEEK2_INTEGRATION_SUMMARY.md`, …) and the ad-hoc `test_*.sh`
scripts that quote the removed flags are records of that work, not documentation
of the current CLI; they are untouched. The flags are refused by both parsers
now:

```
$ ./build/splash serve-native --help 2>&1 | grep -c kv-swap
0
$ ./build/splash serve-native --help 2>&1 | grep -c enable-weight-prefetch
0
$ ./build/splash serve-native --help 2>&1 | grep -c prefetch-layers
0
```

## 6. Verification

| | |
|---|---|
| `make -j8` | clean, 0 warnings |
| `make test-engine-cpu` | 36 of 37 tests pass; the 37th, `gguf-planner`, fails as at `c8b5d3dc` |
| `ane-ffn-startup` | passes |
| `engine-memory-plan` | passes (did not link before) |
| `kv-page-cache`, `engine-cache`, `persistent-cache` | pass (did not link before) |
| `from server import serve_options` | imports |
| lines | +44 / −2,180 in `runtime/`, `server/`, `dev/`, `Makefile`; a further −1,520 of unbuilt `src/` and `tests/` scratch and `verify_phase3.sh` |

`gguf-planner` fails 32 assertions at `c8b5d3dc` as well, with
`blk.0.ffn_gate.weight must be PQ2_0, PTQ1_0 or Q4_K, got 5` on every `qwen35`
dense case: the loader accepts three FFN formats that the planner's fixtures do
not use. That is a separate pre-existing defect and is not touched here.
