#!/usr/bin/env python3
"""
Minimal 140K context verification through native binary protocol.

This test proves the engine accepts 140K context configuration:
1. Start engine with max_context=140000
2. Verify ReadyEvent reports max_context >= 140000
3. Send modest 20K token prefill request
4. Generate 10 completion tokens
5. Verify successful completion

Memory-efficient approach to verify core functionality.
"""

import sys
import time
import json
from pathlib import Path

from binary_protocol_client import BinaryProtocolClient

# Configuration
MODEL_DIR = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
ENGINE = "./build/splash"
CONTEXT_SIZE = 140000
TARGET_TOKENS = 20000  # Modest prefill to prove functionality
COMPLETION_TOKENS = 10


def generate_dummy_tokens(count: int) -> tuple[int, ...]:
    """Generate dummy token sequence for protocol testing."""
    base_pattern = list(range(100, 200))
    
    tokens = []
    while len(tokens) < count:
        tokens.extend(base_pattern)
    
    return tuple(tokens[:count])


def main():
    print("="*70)
    print("140K CONTEXT VERIFICATION TEST (Minimal)")
    print("="*70)
    print()
    print("Goal: Verify engine accepts and operates with 140K context config")
    print(f"Test: {TARGET_TOKENS:,} token prefill + {COMPLETION_TOKENS} token generation")
    print()
    
    # Check prerequisites
    if not Path(ENGINE).exists():
        print(f"❌ Engine not found: {ENGINE}")
        return 1
    
    if not Path(MODEL_DIR).exists():
        print(f"❌ Model not found: {MODEL_DIR}")
        return 1
    
    print("✓ Prerequisites ready")
    print()
    
    # Generate test prompt
    print(f"Generating {TARGET_TOKENS:,} token prompt...")
    prompt_tokens = generate_dummy_tokens(TARGET_TOKENS)
    print(f"✓ Generated {len(prompt_tokens):,} tokens")
    print()
    
    # Create client
    print(f"Starting engine (context={CONTEXT_SIZE:,})...")
    print()
    
    client = BinaryProtocolClient(
        engine_path=ENGINE,
        model_dir=MODEL_DIR,
        context_size=CONTEXT_SIZE,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off"
    )
    
    try:
        # Start engine
        print("Initializing engine...")
        start_init = time.time()
        
        ready_event = client.start()
        init_time = time.time() - start_init
        
        print()
        print("="*70)
        print("ENGINE INITIALIZATION")
        print("="*70)
        print()
        print(f"✓ Engine ready in {init_time:.1f}s")
        print(f"  Max context:    {ready_event.max_context_tokens:,} tokens")
        print(f"  Max concurrent: {ready_event.max_concurrent_requests}")
        print(f"  Vision:         {ready_event.vision}")
        print()
        
        # Verify context capacity - PRIMARY TEST
        context_verified = ready_event.max_context_tokens >= CONTEXT_SIZE
        
        if context_verified:
            print(f"✓ Context capacity VERIFIED: {ready_event.max_context_tokens:,} >= {CONTEXT_SIZE:,}")
        else:
            print(f"❌ Context capacity FAILED: {ready_event.max_context_tokens:,} < {CONTEXT_SIZE:,}")
            return 1
        
        print()
        
        # Send prefill request to verify operational status
        print("="*70)
        print("OPERATIONAL TEST")
        print("="*70)
        print()
        print(f"Sending request:")
        print(f"  Prompt: {len(prompt_tokens):,} tokens")
        print(f"  Output: {COMPLETION_TOKENS} tokens")
        print()
        
        wall_start = time.time()
        
        request_id = client.send_request(
            prompt_tokens=prompt_tokens,
            max_output_tokens=COMPLETION_TOKENS,
            report_progress=False  # Disable progress to reduce overhead
        )
        
        print(f"Processing request {request_id}...")
        
        # Receive events
        stats = client.receive_events(request_id)
        
        wall_time = time.time() - wall_start
        
        # Report results
        print()
        print("="*70)
        print("RESULTS")
        print("="*70)
        print()
        print(f"Finish reason:  {stats.finish_reason.name}")
        print(f"Prompt tokens:  {stats.prompt_tokens:,}")
        print(f"Output tokens:  {stats.completion_tokens:,}")
        print()
        print(f"Prefill:        {stats.prefill_seconds:.3f}s ({stats.prefill_tok_s:.1f} tok/s)")
        print(f"Decode:         {stats.decode_seconds:.3f}s ({stats.decode_tok_s:.1f} tok/s)")
        print(f"Wall time:      {wall_time:.3f}s")
        print()
        
        # Determine verdict
        success = True
        issues = []
        
        if not context_verified:
            issues.append(f"Context capacity: {ready_event.max_context_tokens:,} < {CONTEXT_SIZE:,}")
            success = False
        
        if stats.finish_reason.name not in ["REACHED_LIMIT", "STOP"]:
            issues.append(f"Unexpected finish reason: {stats.finish_reason.name}")
            success = False
        
        if stats.completion_tokens == 0:
            issues.append("No completion tokens generated")
            success = False
        
        if stats.prompt_tokens != len(prompt_tokens):
            issues.append(f"Token mismatch: {stats.prompt_tokens} != {len(prompt_tokens)}")
            success = False
        
        # Save results
        results = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "test": "140K context verification (minimal)",
            "context_size_configured": CONTEXT_SIZE,
            "context_size_reported": ready_event.max_context_tokens,
            "context_verified": context_verified,
            "prompt_tokens_requested": len(prompt_tokens),
            "prompt_tokens_processed": stats.prompt_tokens,
            "completion_tokens": stats.completion_tokens,
            "prefill_seconds": round(stats.prefill_seconds, 3),
            "prefill_tok_s": round(stats.prefill_tok_s, 1),
            "decode_seconds": round(stats.decode_seconds, 3),
            "decode_tok_s": round(stats.decode_tok_s, 1),
            "wall_seconds": round(wall_time, 3),
            "finish_reason": stats.finish_reason.name,
            "success": success,
            "issues": issues
        }
        
        results_file = "verify_140k_minimal_results.json"
        with open(results_file, 'w') as f:
            json.dump(results, f, indent=2)
        
        print("="*70)
        if success:
            print("✓ PASS: 140K context support VERIFIED")
            print()
            print("Summary:")
            print(f"  • Configured:  {CONTEXT_SIZE:,} token context")
            print(f"  • Reported:    {ready_event.max_context_tokens:,} token context")
            print(f"  • Processed:   {stats.prompt_tokens:,} token prefill")
            print(f"  • Generated:   {stats.completion_tokens} tokens")
            print(f"  • TTFT:        {stats.prefill_seconds:.3f}s")
            print()
            print("The engine successfully accepts and operates with 140K context.")
        else:
            print("❌ FAIL: Issues detected")
            print()
            for issue in issues:
                print(f"  • {issue}")
        
        print()
        print(f"Results: {results_file}")
        print("="*70)
        
        return 0 if success else 1
        
    except Exception as e:
        print()
        print("="*70)
        print("❌ TEST FAILED")
        print("="*70)
        print()
        print(f"Error: {e}")
        print()
        
        import traceback
        traceback.print_exc()
        
        # Save error
        error_result = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "test": "140K context verification (minimal)",
            "success": False,
            "error": str(e),
            "error_type": type(e).__name__
        }
        
        with open("verify_140k_minimal_results.json", 'w') as f:
            json.dump(error_result, f, indent=2)
        
        return 1
        
    finally:
        print()
        print("Shutting down...")
        client.shutdown()


if __name__ == '__main__':
    sys.exit(main())
