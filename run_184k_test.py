#!/usr/bin/env python3
"""
184K Context Verification Test Runner
Runs baseline and full context tests, captures metrics.
"""

import subprocess
import time
import json
import sys
import os
from pathlib import Path

MODEL_DIR = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
ENGINE = "./build/splash"
PROMPT_2K = "prompt_2k.txt"
PROMPT_184K = "prompt_184k.txt"

def run_test(context_size, prompt_file, test_name):
    """Run a single test and extract metrics."""
    print(f"\n{'='*70}")
    print(f"{test_name} Test (context={context_size})")
    print(f"{'='*70}\n")
    
    cmd = [
        ENGINE, "serve-native",
        MODEL_DIR,
        str(context_size),
        "auto",
        "--kv-format", "int8",
        "--ane", "off",
        "--idle-release", "off"
    ]
    
    print(f"Command: {' '.join(cmd)}")
    print(f"Prompt: {prompt_file}")
    print(f"Starting test...\n")
    
    start_time = time.time()
    
    try:
        with open(prompt_file, 'r') as f:
            prompt_text = f.read()
        
        # Run the engine
        result = subprocess.run(
            cmd,
            input=prompt_text.encode('utf-8'),
            capture_output=True,
            text=False,
            timeout=600  # 10 minute timeout for large context
        )
        
        end_time = time.time()
        duration = end_time - start_time
        
        print(f"Test completed in {duration:.2f}s")
        print(f"Exit code: {result.returncode}")
        
        # Save raw output
        output_file = f"{test_name}_output.bin"
        with open(output_file, 'wb') as f:
            f.write(result.stdout)
        
        error_file = f"{test_name}_stderr.txt"
        with open(error_file, 'wb') as f:
            f.write(result.stderr)
        
        print(f"Output saved to: {output_file}")
        print(f"Errors saved to: {error_file}")
        
        # Parse stderr for metrics
        stderr_text = result.stderr.decode('utf-8', errors='replace')
        print(f"\n--- Engine stderr (first 3000 chars) ---")
        print(stderr_text[:3000])
        print(f"--- End stderr ---\n")
        
        return {
            'duration': duration,
            'exit_code': result.returncode,
            'stderr': stderr_text,
            'stdout_size': len(result.stdout)
        }
        
    except subprocess.TimeoutExpired:
        print(f"❌ Test timed out after 600 seconds")
        return {'error': 'timeout', 'duration': 600}
    except Exception as e:
        print(f"❌ Test failed with error: {e}")
        return {'error': str(e)}

def extract_metrics(test_result):
    """Extract performance metrics from test result."""
    metrics = {}
    
    if 'stderr' not in test_result:
        return metrics
    
    stderr = test_result['stderr']
    
    # Parse engine output for metrics
    # Look for patterns like:
    # - "prefill: 1234 tokens in 5.67s (217.8 tok/s)"
    # - "decode: 50 tokens in 2.34s (21.4 tok/s)"
    
    import re
    
    # Try to extract prefill metrics
    prefill_match = re.search(r'prefill.*?(\d+\.?\d*)\s*tok/s', stderr, re.IGNORECASE)
    if prefill_match:
        metrics['prefill_toks'] = float(prefill_match.group(1))
    
    # Try to extract decode metrics
    decode_match = re.search(r'decode.*?(\d+\.?\d*)\s*tok/s', stderr, re.IGNORECASE)
    if decode_match:
        metrics['decode_toks'] = float(decode_match.group(1))
    
    return metrics

def main():
    print("="*70)
    print("184K Context Verification Test")
    print("="*70)
    
    # Check prerequisites
    if not os.path.exists(ENGINE):
        print(f"❌ Engine not found: {ENGINE}")
        print("Run: make -j8")
        return 1
    
    if not os.path.exists(MODEL_DIR):
        print(f"❌ Model not found: {MODEL_DIR}")
        return 1
    
    if not os.path.exists(PROMPT_2K) or not os.path.exists(PROMPT_184K):
        print("❌ Prompts not found, generating...")
        subprocess.run([sys.executable, "test_184k_simple.py"], check=True)
    
    print("✓ All prerequisites ready\n")
    
    results = {}
    
    # Run baseline test
    print("\n" + "="*70)
    print("BASELINE TEST (2K Context)")
    print("="*70)
    baseline_result = run_test(2048, PROMPT_2K, "baseline_2k")
    baseline_metrics = extract_metrics(baseline_result)
    results['baseline'] = {**baseline_result, **baseline_metrics}
    
    # Run full test
    print("\n" + "="*70)
    print("FULL TEST (184K Context)")
    print("="*70)
    full_result = run_test(184144, PROMPT_184K, "full_184k")
    full_metrics = extract_metrics(full_result)
    results['full'] = {**full_result, **full_metrics}
    
    # Save results
    results_file = "verify_184k_results.json"
    with open(results_file, 'w') as f:
        json.dump(results, f, indent=2)
    
    print("\n" + "="*70)
    print("RESULTS SUMMARY")
    print("="*70)
    print(f"\nResults saved to: {results_file}")
    print(f"\nBaseline (2K): {json.dumps(results['baseline'], indent=2)}")
    print(f"\nFull (184K): {json.dumps(results['full'], indent=2)}")
    
    # Calculate decode cost if available
    if 'decode_toks' in baseline_metrics and 'decode_toks' in full_metrics:
        baseline_decode = baseline_metrics['decode_toks']
        full_decode = full_metrics['decode_toks']
        ratio = (full_decode / baseline_decode) * 100
        cost = 100 - ratio
        
        print(f"\n" + "="*70)
        print("PERFORMANCE COMPARISON")
        print("="*70)
        print(f"Baseline decode: {baseline_decode:.2f} tok/s")
        print(f"Full decode: {full_decode:.2f} tok/s")
        print(f"Ratio: {ratio:.1f}%")
        print(f"Decode cost: {cost:.1f}%")
        
        if cost < 10:
            print(f"✓ PASS: Decode cost {cost:.1f}% < 10% threshold")
        else:
            print(f"❌ FAIL: Decode cost {cost:.1f}% > 10% threshold")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
