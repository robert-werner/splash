#!/usr/bin/env python3
"""
Test prefill chunking TTFT using binary protocol client.
Measures actual TTFT and total prefill time with 180K prompt.
"""

import sys
import time
import json
from pathlib import Path

# Import binary protocol client
from binary_protocol_client import BinaryProtocolClient, RequestStats

MODEL_DIR = "./install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
ENGINE = "./build/splash"
PROMPT_FILE = "prompt_180k.txt"


def load_prompt() -> str:
    """Load the 180K prompt from file."""
    with open(PROMPT_FILE, 'r') as f:
        return f.read()


def tokenize_prompt(text: str) -> tuple[int, ...]:
    """Tokenize the prompt text - estimate based on char count."""
    # For testing: generate dummy tokens based on the typical 1.3 chars/token ratio
    # This is a simplified approach to avoid tokenizer dependency issues
    estimated_token_count = int(len(text) / 1.3)
    
    print(f"Estimating {len(text):,} chars → ~{estimated_token_count:,} tokens", file=sys.stderr)
    print("(Using dummy token IDs for protocol test)", file=sys.stderr)
    
    # Generate dummy token sequence (repeating pattern for testing)
    # Real tokens aren't critical for measuring TTFT timing
    dummy_tokens = tuple(range(100, 100 + min(estimated_token_count, 200000)))
    
    print(f"✓ Generated {len(dummy_tokens):,} test tokens", file=sys.stderr)
    return dummy_tokens


def main():
    print("="*80)
    print("PREFILL CHUNKING TTFT TEST: 180K PROMPT (BINARY PROTOCOL)")
    print("="*80)
    print()
    
    # Verify prerequisites
    if not Path(ENGINE).exists():
        print(f"❌ Engine not found: {ENGINE}")
        return 1
    
    if not Path(MODEL_DIR).exists():
        print(f"❌ Model not found: {MODEL_DIR}")
        return 1
    
    if not Path(PROMPT_FILE).exists():
        print(f"❌ Prompt not found: {PROMPT_FILE}")
        return 1
    
    if not Path("binary_protocol_client.py").exists():
        print("❌ Binary protocol client not found")
        return 1
    
    print(f"✓ Engine: {ENGINE}")
    print(f"✓ Model: Ternary-Bonsai-2-27B PQ2_0")
    print(f"✓ Prompt file: {PROMPT_FILE}")
    print(f"✓ Binary protocol client available")
    print()
    
    # Load prompt
    print("Loading prompt...")
    prompt_text = load_prompt()
    print(f"✓ Loaded: {len(prompt_text):,} chars")
    print()
    
    # Tokenize prompt
    prompt_tokens = tokenize_prompt(prompt_text)
    num_tokens = len(prompt_tokens)
    
    # Calculate expected chunking behavior
    chunk_size = 8192
    threshold = 16384
    expected_chunks = (num_tokens + chunk_size - 1) // chunk_size if num_tokens > threshold else 1
    
    print()
    print("="*80)
    print("EXPECTED BEHAVIOR")
    print("="*80)
    print()
    print(f"Prompt tokens: {num_tokens:,}")
    print(f"Chunking threshold: {threshold:,} tokens")
    
    if num_tokens > threshold:
        print(f"Expected chunks: {expected_chunks} × {chunk_size:,} tokens")
        print(f"Expected TTFT: ~{chunk_size / 420:.1f}s (first chunk @ 420 tok/s)")
        print(f"Expected total prefill: ~{num_tokens / 420:.1f}s ({num_tokens / 420 / 60:.1f}min)")
    else:
        print(f"No chunking (below threshold)")
        print(f"Expected TTFT: ~{num_tokens / 420:.1f}s")
    
    print(f"Baseline (no chunking): ~{num_tokens / 207:.1f}s ({num_tokens / 207 / 60:.1f}min)")
    print()
    
    # Create client
    print("="*80)
    print("STARTING ENGINE")
    print("="*80)
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
        # Start engine
        ready_event = client.start()
        print()
        print(f"✓ Engine ready")
        print(f"  Max context: {ready_event.max_context_tokens:,} tokens")
        print(f"  Max concurrent: {ready_event.max_concurrent_requests}")
        print()
        
        # Send request with progress reporting
        print("="*80)
        print("RUNNING TEST")
        print("="*80)
        print()
        print(f"Sending request: {num_tokens:,} tokens")
        print("Measuring TTFT...")
        print()
        
        start_time = time.time()
        
        request_id = client.send_request(
            prompt_tokens=prompt_tokens,
            max_output_tokens=10,
            report_progress=True,  # Enable progress events
        )
        
        # Receive events and capture timing
        stats = client.receive_events(request_id)
        
        wall_time = time.time() - start_time
        
        print()
        print("="*80)
        print("RESULTS")
        print("="*80)
        print()
        
        # Display results
        print(f"Request ID: {stats.request_id}")
        print(f"Finish reason: {stats.finish_reason.name}")
        print()
        
        print(f"Tokens:")
        print(f"  Prompt: {stats.prompt_tokens:,}")
        print(f"  Completion: {stats.completion_tokens}")
        print(f"  Matched prefix: {stats.matched_prefix_tokens}")
        print()
        
        print(f"Timing:")
        print(f"  Prefill: {stats.prefill_seconds:.2f}s ({stats.prefill_tok_s:.1f} tok/s)")
        print(f"  Decode: {stats.decode_seconds:.3f}s ({stats.decode_tok_s:.1f} tok/s)")
        print(f"  Wall time: {wall_time:.2f}s")
        print()
        
        # Calculate baseline and improvement
        baseline_time = stats.prompt_tokens / 207
        
        print(f"Performance Analysis:")
        print(f"  Baseline TTFT (207 tok/s): {baseline_time:.1f}s ({baseline_time/60:.1f}min)")
        print(f"  Actual TTFT: {stats.prefill_seconds:.1f}s ({stats.prefill_seconds/60:.2f}min)")
        
        if stats.prefill_seconds > 0:
            speedup = baseline_time / stats.prefill_seconds
            ttft_reduction = ((baseline_time - stats.prefill_seconds) / baseline_time) * 100
            print(f"  Speedup: {speedup:.2f}x")
            print(f"  TTFT reduction: {ttft_reduction:.1f}%")
        
        print()
        
        # Determine success
        target_ttft = 60.0  # 60s with 3× margin on 20s target
        target_reduction = 90.0  # 90% reduction
        target_throughput = 350.0  # 350 tok/s
        
        success_criteria = {
            'ttft_under_60s': stats.prefill_seconds < target_ttft,
            'reduction_over_90pct': ttft_reduction >= target_reduction if stats.prefill_seconds > 0 else False,
            'throughput_over_350': stats.prefill_tok_s >= target_throughput,
        }
        
        all_success = all(success_criteria.values())
        
        print("Success Criteria:")
        for criterion, passed in success_criteria.items():
            status = "✅" if passed else "❌"
            print(f"  {status} {criterion.replace('_', ' ')}")
        print()
        
        if all_success:
            print("🎉 ALL SUCCESS CRITERIA MET!")
        elif success_criteria['ttft_under_60s']:
            print("⚠️  PARTIAL SUCCESS: TTFT < 60s but targets not fully met")
        else:
            print("❌ CHUNKING NOT WORKING: TTFT too high")
        
        print()
        
        # Save results
        result = {
            'test_name': 'chunked_180k_binary_protocol',
            'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
            'commit': '52cf7a4d',
            'prompt_chars': len(prompt_text),
            'prompt_tokens': stats.prompt_tokens,
            'completion_tokens': stats.completion_tokens,
            'expected_chunks': expected_chunks,
            'prefill_seconds': stats.prefill_seconds,
            'prefill_throughput_toks': stats.prefill_tok_s,
            'decode_seconds': stats.decode_seconds,
            'decode_throughput_toks': stats.decode_tok_s,
            'wall_seconds': wall_time,
            'baseline_time_s': baseline_time,
            'speedup': baseline_time / stats.prefill_seconds if stats.prefill_seconds > 0 else 0,
            'ttft_reduction_pct': ttft_reduction if stats.prefill_seconds > 0 else 0,
            'success_criteria': success_criteria,
            'all_success': all_success,
            'finish_reason': stats.finish_reason.name,
            'generated_token_count': len(stats.generated_tokens),
        }
        
        results_file = "test_chunking_ttft_binary_results.json"
        with open(results_file, 'w') as f:
            json.dump(result, f, indent=2)
        
        print(f"Results saved to: {results_file}")
        print()
        
        return 0 if all_success else 1
        
    except Exception as e:
        print()
        print(f"❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 1
        
    finally:
        print("Shutting down engine...")
        client.shutdown()
        print("✓ Engine stopped")


if __name__ == "__main__":
    sys.exit(main())
