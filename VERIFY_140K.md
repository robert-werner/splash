# 140K Context Verification Test

## Objective

Verify that 140K context window provides acceptable performance with decode cost <10% compared to baseline.

**Target context:** 140K tokens (leaves 4 GB safety margin on 24GB M5 Pro)  
**Previous blocker:** 184K required 11.5 GB, only 3.5 GB available

## Test Configuration

| Test | Context Size | Prompt File | Prompt Tokens | Max Output |
|------|--------------|-------------|---------------|------------|
| Baseline | 512 | prompt_2k.txt | ~2,215 | 50 |
| Reduced | 140,000 | prompt_140k.txt | ~139,902 | 50 |

### Model Configuration
- **Model:** Ternary-Bonsai-2-27B PQ2_0
- **Path:** `install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6`
- **Engine:** `./build/splash`
- **KV Format:** int8
- **ANE:** off
- **Memory:** auto

## Test Results

### Status: BLOCKED - Insufficient Memory

Both baseline and reduced context tests failed to start due to memory pressure.

#### Baseline Test (512 context)
```
Error: not enough free memory to start
- Reclaimable: 1896 MiB
- Protected for macOS: 2048 MiB
- System pressure: warning
```

#### Reduced Test (140K context)
```
Error: not enough free memory to start
- Reclaimable: 1868 MiB
- Protected for macOS: 2048 MiB
- System pressure: critical
```

### Memory Requirements

The engine could not start even for the baseline 512-token context test, indicating system-wide memory pressure unrelated to the specific context size being tested.

**Root cause:** Insufficient available memory on the system at test time.

## Performance Metrics

| Metric | Baseline | Reduced (140K) | Ratio | Cost |
|--------|----------|----------------|-------|------|
| Prefill (tok/s) | N/A | N/A | N/A | N/A |
| Decode (tok/s) | N/A | N/A | N/A | N/A |
| Decode Cost | N/A | N/A | N/A | N/A |

## Verdict

**Status:** INCOMPLETE - Memory blocker

The test infrastructure is ready but cannot execute due to system memory pressure. To complete verification:

1. Close memory-intensive applications
2. Ensure sufficient free memory (>4 GB recommended)
3. Re-run: `./run_140k_binary_test.py`

**Pass criteria:** Decode cost < 10%

## Test Artifacts

- `prompt_140k.txt` - Generated test prompt (~140K tokens)
- `run_140k_binary_test.py` - Test runner script
- `binary_protocol_client.py` - Binary protocol client (from sibling task)

## Next Steps

1. Free up system memory
2. Execute verification test
3. Update this document with actual performance metrics
4. Determine PASS/FAIL based on decode cost threshold
