# TTFT Large Prefill Test Results

## Test Configuration

**Date**: 2026-10-20
**Model**: Splash baseline assembly (93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6)
**Context Size**: 40,000 tokens
**KV Format**: int8
**ANE**: off
**Memory**: auto

## Memory Constraints

Initial attempts with 100K tokens and 180K context failed due to macOS OOM killer:
- 100K target with 180K context: All tests failed (exit code 70 for chunked, -9 for monolithic)
- 75K target with 180K context: OOM killed
- 50K target with 80K context: OOM killed
- 30K target with 80K context: OOM killed

**Final working configuration**: 40K context with 20K and 30K token tests

## Test Results

### 20K Token Prefill (Monolithic)
- **TTFT**: 55.97 seconds
- **Prefill throughput**: 357.4 tok/s
- **Status**: ✓ PASS
- **Generated tokens**: 1

### 30K Token Prefill (Monolithic)
- **TTFT**: 87.05 seconds  
- **Prefill throughput**: 344.6 tok/s
- **Status**: ✓ PASS
- **Generated tokens**: 1

## Key Findings

1. **Memory pressure is critical**: The engine requires ~7GB RAM per instance. With 180K context, the KV cache allocation alone exceeded available memory on this 24GB system.

2. **Sequential execution**: Tests run one at a time with 2-second delays between scenarios to ensure clean memory state.

3. **Timeout increased**: Client timeout raised from 300s to 600s in binary_protocol_client.py (line 294).

4. **Chunking not tested**: The Splash protocol doesn't support prefill-only requests (max_output_tokens=0), making client-side chunking tests infeasible without protocol changes.

5. **Linear scaling**: TTFT scales approximately linearly with token count:
   - 20K tokens: 56s (2.8ms/token)
   - 30K tokens: 87s (2.9ms/token)

## Files Modified

- `binary_protocol_client.py`: Increased global_timeout from 300s to 600s
- `measure_ttft_simple.py`: New simplified test script (monolithic only)
- `measure_chunking_fixed_results.txt`: Test output log
- `measure_chunking_fixed_results.json`: Structured test results

## Recommendations

1. For 100K token prefill tests, either:
   - Use a machine with >32GB RAM
   - Reduce max_concurrent from 4 to 1 in engine config
   - Implement true server-side chunking support

2. The current 40K context size is a good balance for memory-constrained systems while still testing large prefills (20-30K tokens).

3. Prefill throughput (~350 tok/s) is consistent across both test sizes, indicating predictable performance scaling.
