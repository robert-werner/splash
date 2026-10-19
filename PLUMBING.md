# Plumbing changes: SSD KV tier + ANE share CLI

## 1. SSD KV tier CLI probing — Infrastructure ready, tier logic pending

### What was done

**CLI and config:**
- Added `--ssd-kv {on,off}`, `--ssd-kv-path DIRECTORY`, `--hot-tier-tokens TOKENS`, `--max-context-extended TOKENS`, `--prefetch-pages N` in `serve_options.py`
- Forwarded them in `server.py` native command builder
- Cross-checks: `--ssd-kv on` requires `--ssd-kv-path` and `--max-context-extended`; the extended count must exceed the hot count
- Extended parsers (`parse_extended_tokens`, `parse_positive_int`) support K/M suffixes and validate ranges

**Native side:**
- `NativeArguments` parses all five flags (`.hpp` declares, `.cpp` parses and validates)
- `main.mm` wires `maxContextExtended`, `hotTierTokens`, `prefetchPages` into `config.resources` and `config.nativeLoop.engine`
- `RuntimeResources.mm:720-753` creates two `SlotFile` instances (`hotFile`, `coldFile`) when `ssdKvEnabled && maxContextExtended > 0`, and constructs `ExtendedKvTier` with the correct config

**Fixed bug**: `SlotFile::Persistence` for hot/cold tiers was initialized with `labelBytes = 0`, causing `SlotFile::open` to refuse and fall back to "SSD cache KV storage disabled". Fixed by supplying `sizeof(KvBlockLabel)` (matching the pattern from the shared KV/state files) and building distinct tags (`cacheNamespace + "/hot"`, `"/cold"`).

### Verification

1. **Build:** clean `-Werror` build (0 warnings), `make -j8 test-engine-cpu` exit 0 (231 CPU tests passing).
2. **Files created:** Running the server with `--ssd-kv on --ssd-kv-path /tmp/ssdkv-test --max-context-extended 1M --hot-tier-tokens 8K` creates `kv_hot_slots.bin`, `kv_hot_records.bin`, `kv_cold_slots.bin`, `kv_cold_records.bin` in `/tmp/ssdkv-test/`.
3. **No more "disabled" message:** The error "SSD cache KV storage disabled; state storage remains enabled (a persistent slot file needs a label size)" no longer appears.
4. **Tier object exists:** `ExtendedKvTier` is constructed and passed into `RuntimeResources` → `Cache`.

### What is NOT working yet

The **tier transition logic is unimplemented**. `ExtendedKvTier` is compiled and instantiated, but neither `Engine` nor `Cache` contain logic to:
- demote a KV slot from hot to cold when `hotTierTokens` is exceeded,
- promote a slot from cold to hot on access,
- or track token usage against the `hotTierTokens` threshold.

In the test workload (7.2K prompt tokens with `--hot-tier-tokens 8K`), the cold slots file stayed at size 0, and the cold tier was never written to. The infrastructure is ready, but the *policy* that decides when to move a slot between tiers is absent.

**Next step:** Implement the eviction logic in `Cache` or `Engine` that calls `ExtendedKvTier::demoteToCold()` / `promoteToHot()` when token counts cross `hotTierTokens`.

---

## 2. ANE share CLI

**CLI and config:**
- Added `--ane-share SHARE` (double in [0, 1)) and `--ane-minimum-rows ROWS` in `serve_options.py`, with `parse_ane_share` validating the range
- Forwarded in `server.py`
- Cross-check: `--ane-minimum-rows` requires `--ane-share`

**Native side:**
- `NativeArguments` parses both flags (`.hpp` declares, `.cpp` parses and range-checks)
- `main.mm` constructs `AneFfnSetting::Given{share, minimumRows}` and sets `config.resources.aneFfn.given` when the user supplied them, preserving `std::nullopt` when absent (= calibrate)

**Verification:**
1. `./build/splash serve-native --help` lists both flags
2. Range checks work:
   - `--ane-share 0.75` accepted (passes through to `MODEL_DIRECTORY must name`)
   - `--ane-share 1.0` rejected: "ane-share requires a share in [0, 1)"
   - `--ane-share -0.1` rejected: same message
3. Default run (no `--ane-share`) still calibrates (no `given`)

**Integration:** The second subagent (ANE mechanism) owns `AneFfnStartup.*` and will verify that the given share is honored instead of calibrated.

---

## Acceptance

✅ `make -j8` clean, 0 warnings, `-Werror`
✅ `make -j8 test-engine-cpu` exit 0
✅ SSD tier files created, no "disabled" error, `ExtendedKvTier` instantiated
⏳ Tier transition logic (hot↔cold demotion/promotion) requires follow-up work
✅ Both ANE flags surface in `serve-native --help`, range-checked, default calibration preserved

---

## Files changed

```
runtime/engine/NativeArguments.hpp   — declare ssdKv*, aneShare, aneMinimumRows
runtime/engine/NativeArguments.cpp   — parse and validate all 7 new flags
runtime/main.mm                      — wire into config.resources + config.nativeLoop.engine
runtime/engine/RuntimeResources.hpp  — add maxContextExtended field to EngineConfig
runtime/engine/RuntimeResources.mm   — create hot/cold SlotFiles, fix label sizes
server/serve_options.py              — define 7 ServeOptions, parsers, cross-checks
server/server.py                     — forward flags to native command
```

All changes committed on branch `plumbing`.
