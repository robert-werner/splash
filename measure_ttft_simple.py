#!/usr/bin/env python3
"""
Measure TTFT for large token prefill with monolithic approach.

Test matrix:
- 50K tokens
- 75K tokens

For each configuration:
1. Launch server via BinaryProtocolClient
2. Send monolithic prefill
3. Measure time from first request to first generation token
4. Shutdown server
"""

import time
import sys
import os
from pathlib import Path
from typing import Optional, Tuple
import json

# Import the fixed binary protocol client
from binary_protocol_client import BinaryProtocolClient

# Configuration
ASSEMBLY_PATH = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
SERVER_BINARY = "./build/splash"
CONTEXT_SIZE = 40000  # Conservative size for memory-constrained testing

# Token counts to test
TOKEN_COUNTS = [20000, 30000]


def generate_token_sequence(num_tokens: int) -> Tuple[int, ...]:
    """Generate a sequence of token IDs for testing.
    
    Uses a simple pattern: token IDs from 100 to num_tokens+100.
    This avoids special tokens and creates a predictable sequence.
    """
    return tuple(range(100, 100 + num_tokens))


def measure_monolithic_ttft(
    client: BinaryProtocolClient,
    total_tokens: int
) -> Optional[float]:
    """Measure TTFT with single monolithic prefill.
    
    Args:
        client: Connected BinaryProtocolClient instance
        total_tokens: Total tokens to prefill
        
    Returns:
        TTFT in seconds, or None if failed
    """
    try:
        # Generate full token sequence
        all_tokens = generate_token_sequence(total_tokens)
        
        print(f"  Sending monolithic {total_tokens} token prefill...")
        
        start_time = time.time()
        
        # Send full prefill + request 1 generation token
        request_id = client.send_request(
            prompt_tokens=all_tokens,
            max_output_tokens=1,
            report_progress=True,
        )
        
        # Receive events
        stats = client.receive_events(request_id)
        
        print(f"  Prefill: {stats.prefill_seconds:.3f}s ({stats.prefill_tok_s:.1f} tok/s)")
        
        if stats.generated_tokens:
            first_token_time = time.time()
            ttft = first_token_time - start_time
            print(f"  Generated {len(stats.generated_tokens)} token(s)")
            return ttft
        else:
            print("  ERROR: No generation token received")
            return None
            
    except Exception as e:
        print(f"  ERROR: {e}")
        import traceback
        traceback.print_exc()
        return None


def run_test_scenario(total_tokens: int) -> dict:
    """Run a single test scenario.
    
    Args:
        total_tokens: Total tokens to prefill
        
    Returns:
        Dict with test results
    """
    scenario_name = f"{total_tokens//1000}K tokens"
    print(f"\n{'='*60}")
    print(f"Testing: {scenario_name}")
    print(f"{'='*60}")
    
    # Create client and start engine
    client = BinaryProtocolClient(
        engine_path=SERVER_BINARY,
        model_dir=ASSEMBLY_PATH,
        context_size=CONTEXT_SIZE,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off"
    )
    
    try:
        # Start engine
        ready_event = client.start()
        print(f"Engine ready: max_context={ready_event.max_context_tokens}")
        
        # Run measurement
        ttft = measure_monolithic_ttft(client, total_tokens)
        
        result = {
            "scenario": scenario_name,
            "total_tokens": total_tokens,
            "ttft_seconds": ttft,
            "success": ttft is not None
        }
        
        if ttft:
            print(f"✓ TTFT: {ttft:.2f}s")
        else:
            print(f"✗ Test failed")
            
        return result
        
    except Exception as e:
        print(f"✗ Test failed with exception: {e}")
        import traceback
        traceback.print_exc()
        
        return {
            "scenario": scenario_name,
            "total_tokens": total_tokens,
            "ttft_seconds": None,
            "success": False,
            "error": str(e)
        }
        
    finally:
        # Always shutdown engine
        try:
            client.shutdown()
        except:
            pass
        time.sleep(2)  # Brief pause between tests


def main():
    """Run all test scenarios and report results."""
    os.chdir(Path(__file__).parent)
    
    print("="*60)
    print("TTFT Measurement - Large Token Prefill Study")
    print("="*60)
    print(f"Assembly: {ASSEMBLY_PATH}")
    print(f"Token counts: {[f'{tc//1000}K' for tc in TOKEN_COUNTS]}")
    print()
    
    results = []
    
    # Run all test scenarios
    for token_count in TOKEN_COUNTS:
        result = run_test_scenario(token_count)
        results.append(result)
    
    # Summary report
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print(f"{'Scenario':<20} {'TTFT (s)':<10} {'Status'}")
    print("-"*60)
    
    for r in results:
        ttft_str = f"{r['ttft_seconds']:.2f}" if r['ttft_seconds'] else "FAILED"
        status = "✓" if r['success'] else "✗"
        print(f"{r['scenario']:<20} {ttft_str:<10} {status}")
    
    # Save results to JSON
    output_file = "ttft_results.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to: {output_file}")


if __name__ == "__main__":
    main()
