#!/usr/bin/env python3
"""
Test prefill chunking with 30K tokens - smaller test to verify mechanism.
"""

import sys
import time
import json
from pathlib import Path

from binary_protocol_client import BinaryProtocolClient, RequestStats

MODEL_DIR = "./install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
ENGINE = "./build/splash"

def main():
    print("="*80)
    print("PREFILL CHUNKING TEST: 30K TOKENS (VERIFICATION)")
    print("="*80)
    print()
    
    # Generate 30K test tokens (above 16K threshold)
    num_tokens = 30000
    prompt_tokens = tuple(range(100, 100 + num_tokens))
    
    chunk_size = 8192
    threshold = 16384
    expected_chunks = (num_tokens + chunk_size - 1) // chunk_size
    
    print(f"Test tokens: {num_tokens:,}")
    print(f"Chunking threshold: {threshold:,} tokens")
    print(f"Expected chunks: {expected_chunks} × {chunk_size:,} tokens")
    print(f"Expected TTFT: ~{chunk_size / 420:.1f}s (first chunk @ 420 tok/s)")
    print(f"Baseline: ~{num_tokens / 207:.1f}s ({num_tokens / 207 / 60:.1f}min)")
    print()
    
    client = BinaryProtocolClient(
        engine_path=ENGINE,
        model_dir=MODEL_DIR,
        context_size=131072,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off",
    )
    
    try:
        print("Starting engine...")
        ready_event = client.start()
        print(f"✓ Engine ready (max_context={ready_event.max_context_tokens:,})")
        print()
        
        print("Sending request...")
        start_time = time.time()
        
        request_id = client.send_request(
            prompt_tokens=prompt_tokens,
            max_output_tokens=10,
            report_progress=True,
        )
        
        stats = client.receive_events(request_id)
        wall_time = time.time() - start_time
        
        print()
        print("="*80)
        print("RESULTS")
        print("="*80)
        print()
        print(f"Prompt tokens: {stats.prompt_tokens:,}")
        print(f"Prefill: {stats.prefill_seconds:.2f}s ({stats.prefill_tok_s:.1f} tok/s)")
        print(f"Decode: {stats.decode_seconds:.3f}s")
        print(f"Wall time: {wall_time:.2f}s")
        print()
        
        baseline_time = stats.prompt_tokens / 207
        print(f"Baseline (207 tok/s): {baseline_time:.1f}s")
        print(f"Speedup: {baseline_time / stats.prefill_seconds:.2f}x")
        print(f"TTFT reduction: {((baseline_time - stats.prefill_seconds) / baseline_time) * 100:.1f}%")
        print()
        
        if stats.prefill_seconds < 60:
            print("✅ CHUNKING APPEARS TO BE WORKING!")
        else:
            print("⚠️  Slower than expected, check logs")
        
        result = {
            'test_name': 'chunked_30k',
            'prompt_tokens': stats.prompt_tokens,
            'prefill_seconds': stats.prefill_seconds,
            'prefill_tok_s': stats.prefill_tok_s,
            'baseline_time_s': baseline_time,
            'speedup': baseline_time / stats.prefill_seconds,
        }
        
        with open("test_chunking_30k_results.json", 'w') as f:
            json.dump(result, f, indent=2)
        
        return 0
        
    except Exception as e:
        print(f"❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 1
        
    finally:
        print()
        client.shutdown()

if __name__ == "__main__":
    sys.exit(main())
