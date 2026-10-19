#!/usr/bin/env python3
"""
184K Context Verification Test Runner (Binary Protocol)
Runs baseline and full context tests, captures actual metrics.

Requires: binary_protocol_client.py from sibling task.
"""

import sys
import time
import json
import os
from pathlib import Path

# Will be available once sibling task delivers binary_protocol_client.py
try:
    from binary_protocol_client import BinaryProtocolClient
    BINARY_CLIENT_AVAILABLE = True
except ImportError:
    print("⚠️  binary_protocol_client.py not yet available")
    print("    Waiting for sibling task #2 to deliver the module")
    BINARY_CLIENT_AVAILABLE = False

MODEL_DIR = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
ENGINE = "./build/splash"

# Use smaller contexts to avoid memory wait issues
BASELINE_CONTEXT = 512
BASELINE_PROMPT_FILE = "prompt_256.txt"

FULL_CONTEXT = 184144  # Maximum for M5 Pro 24GB
FULL_PROMPT_FILE = "prompt_180k.txt"


def check_prerequisites():
    """Verify all required files and tools are available."""
    issues = []
    
    if not Path(ENGINE).exists():
        issues.append(f"Engine not found: {ENGINE} (run: make -j8)")
    
    if not Path(MODEL_DIR).exists():
        issues.append(f"Model not found: {MODEL_DIR}")
    
    if not Path(BASELINE_PROMPT_FILE).exists():
        issues.append(f"Baseline prompt not found: {BASELINE_PROMPT_FILE}")
    
    if not Path(FULL_PROMPT_FILE).exists():
        issues.append(f"Full prompt not found: {FULL_PROMPT_FILE}")
    
    if not BINARY_CLIENT_AVAILABLE:
        issues.append("binary_protocol_client.py not available (waiting for sibling task)")
    
    return issues


def tokenize_text(text: str) -> tuple[int, ...]:
    """
    Convert text to dummy token sequence for protocol testing.
    Uses character count estimation to generate appropriate token count.
    """
    # Estimate ~1.3 chars per token (typical for English text)
    estimated_tokens = int(len(text) / 1.3)
    
    # Generate dummy tokens (real tokenization not critical for throughput measurement)
    # Use sequential IDs to avoid any vocabulary issues
    dummy_tokens = tuple(range(100, 100 + estimated_tokens))
    
    print(f"  Text: {len(text):,} chars → {len(dummy_tokens):,} tokens (estimated)")
    
    return dummy_tokens


def run_test(context_size: int, prompt_file: str, test_name: str, max_output: int = 50) -> dict:
    """Run a single test and capture metrics."""
    print(f"\n{'='*70}")
    print(f"{test_name.upper()} TEST")
    print(f"{'='*70}\n")
    
    # Load prompt
    print(f"Loading prompt: {prompt_file}")
    with open(prompt_file, 'r') as f:
        prompt_text = f.read()
    
    print(f"Tokenizing...")
    prompt_tokens = tokenize_text(prompt_text)
    
    # Create client
    print(f"\nStarting engine (context={context_size:,})...")
    client = BinaryProtocolClient(
        engine_path=ENGINE,
        model_dir=MODEL_DIR,
        context_size=context_size,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off"
    )
    
    try:
        # Start engine and wait for ready
        ready_event = client.start()
        
        print(f"\n✓ Engine ready:")
        print(f"  Max context: {ready_event.max_context_tokens:,} tokens")
        print(f"  Max concurrent: {ready_event.max_concurrent_requests}")
        print(f"  Vision: {ready_event.vision}")
        
        # Send request
        print(f"\nSending request:")
        print(f"  Prompt: {len(prompt_tokens):,} tokens")
        print(f"  Max output: {max_output} tokens")
        
        start_time = time.time()
        
        request_id = client.send_request(
            prompt_tokens=prompt_tokens,
            max_output_tokens=max_output,
            report_progress=True
        )
        
        print(f"  Request ID: {request_id}")
        print(f"\nGenerating...")
        
        # Receive events and capture stats
        stats = client.receive_events(request_id)
        
        wall_time = time.time() - start_time
        
        # Print results
        print(f"\n{'='*70}")
        print(f"RESULTS: {test_name}")
        print(f"{'='*70}")
        print(f"\nPrompt:     {stats.prompt_tokens:,} tokens")
        print(f"Completion: {stats.completion_tokens:,} tokens")
        print(f"Finish:     {stats.finish_reason.name}")
        print(f"\nPrefill:    {stats.prefill_seconds:.3f}s ({stats.prefill_tok_s:.1f} tok/s)")
        print(f"Decode:     {stats.decode_seconds:.3f}s ({stats.decode_tok_s:.1f} tok/s)")
        print(f"Wall:       {wall_time:.3f}s")
        
        if stats.completion_tokens > 0:
            print(f"\nGenerated {stats.completion_tokens} tokens")
        
        return {
            'test_name': test_name,
            'context_size': context_size,
            'prompt_tokens': stats.prompt_tokens,
            'completion_tokens': stats.completion_tokens,
            'prefill_seconds': stats.prefill_seconds,
            'prefill_tok_s': stats.prefill_tok_s,
            'decode_seconds': stats.decode_seconds,
            'decode_tok_s': stats.decode_tok_s,
            'wall_seconds': wall_time,
            'finish_reason': stats.finish_reason.name,
            'matched_prefix': stats.matched_prefix_tokens,
            'success': True
        }
        
    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        
        return {
            'test_name': test_name,
            'context_size': context_size,
            'success': False,
            'error': str(e)
        }
    
    finally:
        print(f"\nShutting down engine...")
        client.shutdown()


def main():
    print("="*70)
    print("184K CONTEXT END-TO-END VERIFICATION")
    print("="*70)
    print()
    
    # Check prerequisites
    issues = check_prerequisites()
    if issues:
        print("❌ Prerequisites not met:\n")
        for issue in issues:
            print(f"   • {issue}")
        print()
        
        if not BINARY_CLIENT_AVAILABLE:
            print("This test requires binary_protocol_client.py from sibling task #2.")
            print("Once available, re-run this script.")
        
        return 1
    
    print("✓ All prerequisites ready\n")
    
    results = {}
    
    # Run baseline test
    print("\n" + "="*70)
    print("PHASE 1: BASELINE TEST")
    print("="*70)
    baseline = run_test(
        context_size=BASELINE_CONTEXT,
        prompt_file=BASELINE_PROMPT_FILE,
        test_name="baseline",
        max_output=50
    )
    results['baseline'] = baseline
    
    # Run full test
    print("\n" + "="*70)
    print("PHASE 2: FULL CONTEXT TEST")
    print("="*70)
    full = run_test(
        context_size=FULL_CONTEXT,
        prompt_file=FULL_PROMPT_FILE,
        test_name="full_184k",
        max_output=50
    )
    results['full'] = full
    
    # Save results
    results_file = "verify_184k_binary_results.json"
    with open(results_file, 'w') as f:
        json.dump(results, f, indent=2)
    
    print(f"\n{'='*70}")
    print("PERFORMANCE COMPARISON")
    print(f"{'='*70}\n")
    
    if baseline['success'] and full['success']:
        baseline_decode = baseline['decode_tok_s']
        full_decode = full['decode_tok_s']
        
        ratio = (full_decode / baseline_decode) * 100
        cost = 100 - ratio
        
        print(f"Baseline decode:    {baseline_decode:.1f} tok/s")
        print(f"Full (184K) decode: {full_decode:.1f} tok/s")
        print(f"Ratio:              {ratio:.1f}%")
        print(f"Decode cost:        {cost:.1f}%")
        print()
        
        if cost < 10:
            print(f"✓ PASS: Decode cost {cost:.1f}% < 10% threshold")
            verdict = "PASS"
        else:
            print(f"❌ FAIL: Decode cost {cost:.1f}% ≥ 10% threshold")
            verdict = "FAIL"
        
        results['verdict'] = verdict
        results['decode_cost_percent'] = cost
        
        # Update results file with verdict
        with open(results_file, 'w') as f:
            json.dump(results, f, indent=2)
        
        print(f"\nResults saved to: {results_file}")
        print(f"\nFinal Verdict: {verdict}")
        
        return 0 if verdict == "PASS" else 1
    
    else:
        print("❌ One or both tests failed - cannot calculate performance comparison")
        return 1


if __name__ == '__main__':
    if not BINARY_CLIENT_AVAILABLE:
        print("\n⚠️  Cannot run tests: binary_protocol_client.py not available yet")
        print("    This module will be delivered by sibling task #2")
        sys.exit(1)
    
    sys.exit(main())
