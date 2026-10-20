#!/usr/bin/env python3
"""
Measure TTFT for 100K token prefill with different chunk sizes.

Test matrix:
- 8K chunks
- 16K chunks  
- 32K chunks
- No chunking (100K single prefill)

For each configuration:
1. Launch server via BinaryProtocolClient
2. Send chunked or monolithic prefill
3. Measure time from first request to first generation token
4. Shutdown server
"""

import time
import sys
import os
from pathlib import Path
from typing import Optional, List, Tuple
import json

# Import the fixed binary protocol client
from binary_protocol_client import BinaryProtocolClient

# Configuration
ASSEMBLY_PATH = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
SERVER_BINARY = "./build/splash"
CONTEXT_SIZE = 180000
TARGET_TOKENS = 75000  # 75K tokens for prefill (reduced from 100K due to memory constraints)

# Chunk sizes to test (in tokens)
CHUNK_SIZES = [8192, 16384, 32768, None]  # None = no chunking

# Delay between chunks (seconds) - to simulate client-side chunking
CHUNK_DELAY = 0.05


def generate_token_sequence(num_tokens: int) -> Tuple[int, ...]:
    """Generate a sequence of token IDs for testing.
    
    Uses a simple pattern: token IDs from 100 to num_tokens+100.
    This avoids special tokens and creates a predictable sequence.
    """
    return tuple(range(100, 100 + num_tokens))


def measure_chunked_ttft(
    client: BinaryProtocolClient,
    chunk_size: int,
    total_tokens: int
) -> Optional[float]:
    """Measure TTFT with client-side chunked prefill.
    
    Sends multiple sequential requests to simulate chunking.
    
    Args:
        client: Connected BinaryProtocolClient instance
        chunk_size: Size of each chunk in tokens
        total_tokens: Total tokens to prefill
        
    Returns:
        TTFT in seconds, or None if failed
    """
    try:
        # Generate full token sequence
        all_tokens = generate_token_sequence(total_tokens)
        
        # Calculate number of chunks
        num_chunks = (total_tokens + chunk_size - 1) // chunk_size
        
        print(f"  Sending {num_chunks} chunks of ~{chunk_size} tokens each...")
        
        start_time = time.time()
        first_token_time = None
        
        # Send chunks sequentially
        for i in range(num_chunks):
            chunk_start = i * chunk_size
            chunk_end = min((i + 1) * chunk_size, total_tokens)
            chunk_tokens = all_tokens[chunk_start:chunk_end]
            
            print(f"  Chunk {i+1}/{num_chunks}: tokens {chunk_start}-{chunk_end} ({len(chunk_tokens)} tokens)")
            
            # All chunks request 1 generation token
            # (protocol doesn't support max_output_tokens=0)
            is_last = (i == num_chunks - 1)
            max_output = 1
            
            request_id = client.send_request(
                prompt_tokens=chunk_tokens,
                max_output_tokens=max_output,
                report_progress=True,
            )
            
            # Receive events for this request
            stats = client.receive_events(request_id)
            
            print(f"    Prefill: {stats.prefill_seconds:.3f}s ({stats.prefill_tok_s:.1f} tok/s)")
            
            if is_last and stats.generated_tokens:
                # Record time of first generation token
                first_token_time = time.time()
                print(f"    Generated {len(stats.generated_tokens)} token(s)")
            
            # Small delay between chunks
            if not is_last:
                time.sleep(CHUNK_DELAY)
        
        if first_token_time:
            ttft = first_token_time - start_time
            return ttft
        else:
            print("  ERROR: No generation token received")
            return None
            
    except Exception as e:
        print(f"  ERROR: {e}")
        import traceback
        traceback.print_exc()
        return None


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


def run_test_scenario(chunk_size: Optional[int], total_tokens: int) -> dict:
    """Run a single test scenario.
    
    Args:
        chunk_size: Chunk size in tokens, or None for monolithic
        total_tokens: Total tokens to prefill
        
    Returns:
        Dict with test results
    """
    scenario_name = f"{'Monolithic' if chunk_size is None else f'{chunk_size//1024}K chunks'}"
    print(f"\n{'='*60}")
    print(f"Testing: {scenario_name} ({total_tokens} tokens)")
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
        if chunk_size is None:
            ttft = measure_monolithic_ttft(client, total_tokens)
        else:
            ttft = measure_chunked_ttft(client, chunk_size, total_tokens)
        
        result = {
            "scenario": scenario_name,
            "chunk_size": chunk_size,
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
            "chunk_size": chunk_size,
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
    print("TTFT Measurement - 100K Token Prefill Chunking Study")
    print("="*60)
    print(f"Assembly: {ASSEMBLY_PATH}")
    print(f"Target tokens: {TARGET_TOKENS}")
    print(f"Chunk sizes: {[f'{cs//1024}K' if cs else 'None' for cs in CHUNK_SIZES]}")
    print()
    
    results = []
    
    # Run all test scenarios
    for chunk_size in CHUNK_SIZES:
        result = run_test_scenario(chunk_size, TARGET_TOKENS)
        results.append(result)
    
    # Summary report
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print(f"{'Scenario':<20} {'Chunk Size':<12} {'TTFT (s)':<10} {'Status'}")
    print("-"*60)
    
    for r in results:
        chunk_str = f"{r['chunk_size']//1024}K" if r['chunk_size'] else "N/A"
        ttft_str = f"{r['ttft_seconds']:.2f}" if r['ttft_seconds'] else "FAILED"
        status = "✓" if r['success'] else "✗"
        print(f"{r['scenario']:<20} {chunk_str:<12} {ttft_str:<10} {status}")
    
    # Find optimal
    successful_results = [r for r in results if r['success']]
    if successful_results:
        optimal = min(successful_results, key=lambda x: x['ttft_seconds'])
        print(f"\nOptimal configuration: {optimal['scenario']}")
        print(f"TTFT: {optimal['ttft_seconds']:.2f}s")
        
        # Calculate improvements vs baseline
        monolithic = next((r for r in results if r['chunk_size'] is None and r['success']), None)
        if monolithic and optimal['chunk_size'] is not None:
            improvement = (1 - optimal['ttft_seconds'] / monolithic['ttft_seconds']) * 100
            print(f"Improvement vs monolithic: {improvement:.1f}%")
    
    # Save results to JSON
    output_file = "chunking_ttft_results.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to: {output_file}")


if __name__ == "__main__":
    main()
