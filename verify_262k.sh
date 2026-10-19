#!/bin/bash
set -euo pipefail

# 262K Context Verification Script
# Runs baseline and full context tests, measures performance

MODEL_DIR="${HOME}/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0"
ENGINE="./build/splash"
PROMPT_262K="prompt_262k.txt"
PROMPT_2K="prompt_2k.txt"
RESULTS_FILE="verify_262k_results.json"

echo "======================================================================"
echo "262K Context Verification"
echo "======================================================================"
echo ""

# Check prerequisites
if [[ ! -d "$MODEL_DIR" ]]; then
    echo "❌ Model not found: $MODEL_DIR"
    echo "Expected: Qwen3.8-27B-gguf:PQ2_0"
    echo ""
    echo "To download:"
    echo "  splash serve --model unsloth/Qwen3.8-27B-GGUF:PQ2_0"
    exit 1
fi
echo "✓ Model found: $MODEL_DIR"

if [[ ! -x "$ENGINE" ]]; then
    echo "❌ Engine not found: $ENGINE"
    echo "Run: make -j8"
    exit 1
fi
echo "✓ Engine built: $ENGINE"

if [[ ! -f "$PROMPT_262K" ]]; then
    echo "Generating test prompts..."
    python3 test_262k_simple.py
fi
echo "✓ Test prompts ready"
echo ""

# Function to run a single test
run_test() {
    local context_size=$1
    local prompt_file=$2
    local label=$3
    
    echo "======================================================================"
    echo "$label Test (context=$context_size)"
    echo "======================================================================"
    
    local output_file="${label}_output.txt"
    local metrics_file="${label}_metrics.txt"
    
    # Start engine and capture metrics
    # Note: This is a simplified version; actual implementation would need
    # to parse engine output protocol for precise metrics
    
    echo "Starting engine..."
    local start_time=$(date +%s.%N)
    
    # Run engine (would need proper protocol handling)
    timeout 600 "$ENGINE" serve-native \
        "$MODEL_DIR" \
        "$context_size" \
        "auto" \
        --kv-format int8 \
        --ane off \
        --idle-release off \
        < "$prompt_file" \
        > "$output_file" 2>&1 || {
            echo "❌ Engine failed or timed out"
            return 1
        }
    
    local end_time=$(date +%s.%N)
    local duration=$(echo "$end_time - $start_time" | bc)
    
    # Extract metrics from output
    # This would need proper parsing of engine protocol output
    echo "Duration: ${duration}s" | tee "$metrics_file"
    
    # Check for crashes or errors
    if grep -qi "error\|crash\|oom\|killed" "$output_file"; then
        echo "❌ Detected errors in output"
        return 1
    fi
    
    echo "✓ Test completed"
    echo ""
    return 0
}

# Note: The actual testing requires proper protocol implementation
# For now, document the manual procedure

cat > VERIFY_262K.md << 'EOF'
# 262K Context Verification Report

**Date:** $(date '+%Y-%m-%d %H:%M:%S')
**Model:** Qwen3.8-27B-gguf:PQ2_0 int4
**Branch:** verify-262k
**Commit:** $(git rev-parse HEAD)

## Test Objective

Verify that the Splash inference engine can:
1. Load and process a full 262,144-token context
2. Generate coherent output tokens
3. Maintain acceptable decode performance vs baseline
4. Not crash, OOM, or produce garbage

## Model Configuration

- **Path:** `${HOME}/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0`
- **Format:** GGUF PQ2_0 (int4 quantization)
- **Native context:** 32K (extended to 262K via config)
- **KV cache:** INT8 format
- **ANE:** Disabled (GPU-only for measurement)

## Test Methodology

### Test Prompts

Two prompts generated via `test_262k_simple.py`:

1. **Baseline:** ~2K tokens (8KB text)
2. **Full:** ~262K tokens (1.1MB text)

Both prompts use repetitive diverse-vocabulary paragraphs to:
- Avoid compression artifacts
- Test true memory/attention cost
- Verify output coherence with known content

### Measurement Protocol

For each test (baseline and full):

1. Start engine with specified max-context
2. Load prompt and measure prefill time/speed
3. Generate 50+ tokens
4. Measure decode tok/s
5. Verify output coherence (no garbage)
6. Check for crashes/OOM/errors

### Expected Results

Based on model budget analysis (~6% decode cost increase):

- **Baseline (2K):** 
  - Prefill: 300-400 tok/s
  - Decode: 70-80 tok/s
  
- **Full (262K):**
  - Prefill: 50-100 tok/s (lower due to volume)
  - Decode: 65-75 tok/s (target: <10% degradation)
  - Memory: <40GB peak

## Manual Test Procedure

Since the engine uses a custom protocol, follow these steps:

### 1. Baseline Test (2K context)

\`\`\`bash
# Terminal 1: Start engine
./build/splash serve-native \\
  "${HOME}/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0" \\
  2048 \\
  auto \\
  --kv-format int8 \\
  --ane off \\
  --idle-release off

# Terminal 2: Send prompt (via proper protocol)
# Record: prefill time, prefill tok/s, decode tok/s, memory usage
\`\`\`

### 2. Full Test (262K context)

\`\`\`bash
# Terminal 1: Start engine with 262K context
./build/splash serve-native \\
  "${HOME}/.cache/splash/models/unsloth/Qwen3.8-27B-GGUF/PQ2_0" \\
  262144 \\
  auto \\
  --kv-format int8 \\
  --ane off \\
  --idle-release off

# Terminal 2: Send full prompt
# Record: prefill time, prefill tok/s, decode tok/s, memory usage
\`\`\`

### 3. Measurements

Track these metrics via engine output:

- **Prefill phase:**
  - Time (seconds)
  - Tokens per second
  - Peak memory (Metal + system)

- **Decode phase:**
  - Per-token latency
  - Tokens per second (average over 50 tokens)
  - Memory footprint

- **Quality:**
  - Output coherence (visual inspection)
  - No crashes, OOM, or hangs

## Results

### Baseline (2K context)

| Metric | Value |
|--------|-------|
| Prompt tokens | ~2,000 |
| Prefill time | [TODO: measure] |
| Prefill tok/s | [TODO: measure] |
| Generated tokens | 50+ |
| Decode tok/s | [TODO: measure] |
| Peak memory | [TODO: measure] |
| Status | [✓ PASS / ❌ FAIL] |

### Full (262K context)

| Metric | Value |
|--------|-------|
| Prompt tokens | ~262,144 |
| Prefill time | [TODO: measure] |
| Prefill tok/s | [TODO: measure] |
| Generated tokens | 50+ |
| Decode tok/s | [TODO: measure] |
| Peak memory | [TODO: measure] |
| Status | [✓ PASS / ❌ FAIL] |

### Comparison

| Metric | Baseline | Full | Ratio | Cost |
|--------|----------|------|-------|------|
| Decode tok/s | [TODO] | [TODO] | [TODO]% | [TODO]% |
| Expected cost | - | - | ~94% | ~6% |

## Verdict

**Status:** [✓ PASS / ❌ FAIL / ⏳ IN PROGRESS]

### Pass Criteria

- [✓ / ❌] Engine loads 262K context without crash
- [✓ / ❌] Prefill completes successfully
- [✓ / ❌] Generates 50+ coherent tokens
- [✓ / ❌] Decode cost < 10% (tok/s ratio > 90%)
- [✓ / ❌] No OOM or memory corruption
- [✓ / ❌] Output quality acceptable

### Issues Encountered

[Document any problems, workarounds, or unexpected behavior]

## Files Generated

- `prompt_262k.txt` - Full 262K-token prompt
- `prompt_2k.txt` - Baseline 2K-token prompt
- `test_262k_simple.py` - Prompt generator
- `verify_262k.sh` - Test automation script
- `VERIFY_262K.md` - This report

## Conclusion

[Summary of findings, verdict, and recommendations]

## Next Steps

- [ ] Complete manual measurements
- [ ] Fill in results tables
- [ ] Update verdict section
- [ ] Commit results on verify-262k branch
- [ ] Review before merge consideration

---

**DO NOT MERGE** this branch without reviewing results and obtaining approval.
The 262K context support is experimental and requires validation of:
- Performance characteristics
- Memory behavior
- Output quality
- Stability under load
EOF

echo "✓ Verification document created: VERIFY_262K.md"
echo ""
echo "======================================================================"
echo "Next Steps"
echo "======================================================================"
echo ""
echo "1. Review VERIFY_262K.md for test procedure"
echo "2. Run manual tests following the documented steps"
echo "3. Record measurements in the results tables"
echo "4. Update verdict and commit on verify-262k branch"
echo ""
echo "Test prompts are ready:"
echo "  - prompt_2k.txt (baseline)"
echo "  - prompt_262k.txt (full test)"
echo ""
