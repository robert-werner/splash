#!/usr/bin/env python3
"""
Test binary protocol client with prompt_2k.txt.
Uses existing tokenizer from server package.
"""

import sys
import os

# Ensure we can import from server
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "server"))

from binary_protocol_client import BinaryProtocolClient

# Import tokenizer - try different possible locations
try:
    from server.tokenizer import Tokenizer
except ImportError:
    try:
        import tokenizer
        Tokenizer = tokenizer.Tokenizer
    except ImportError:
        print("Error: Cannot import Tokenizer. Install transformers: pip install transformers", file=sys.stderr)
        sys.exit(1)


def main():
    # Configuration
    engine_path = "build/splash"
    model_dir = os.environ.get("MODEL_DIR")
    
    if model_dir is None:
        # Try to find model directory
        for candidate in ["models/Llama-3.2-1B-Instruct", "models/llama-3.2-1b-instruct"]:
            if os.path.exists(candidate):
                model_dir = candidate
                break
    
    if model_dir is None or not os.path.exists(model_dir):
        print("Error: Model directory not found", file=sys.stderr)
        print("Set MODEL_DIR environment variable or place model in models/", file=sys.stderr)
        sys.exit(1)
    
    prompt_file = "prompt_2k.txt"
    context_size = 8192
    max_output_tokens = 50
    
    if not os.path.exists(engine_path):
        print(f"Error: Engine not found at {engine_path}", file=sys.stderr)
        sys.exit(1)
    
    if not os.path.exists(prompt_file):
        print(f"Error: Prompt file not found: {prompt_file}", file=sys.stderr)
        sys.exit(1)
    
    # Load prompt
    print("Loading prompt...", file=sys.stderr)
    with open(prompt_file, 'r') as f:
        prompt_text = f.read()
    
    print(f"Prompt size: {len(prompt_text)} chars", file=sys.stderr)
    
    # Tokenize
    print("Tokenizing...", file=sys.stderr)
    tokenizer = Tokenizer.from_model(model_dir)
    prompt_tokens = tokenizer.encode(prompt_text)
    
    print(f"Token count: {len(prompt_tokens)}", file=sys.stderr)
    
    # Create client
    print("\nInitializing binary protocol client...", file=sys.stderr)
    client = BinaryProtocolClient(
        engine_path=engine_path,
        model_dir=model_dir,
        context_size=context_size,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off"
    )
    
    try:
        # Start engine
        print("Starting engine...", file=sys.stderr)
        ready_event = client.start()
        
        print(f"\nEngine ready:", file=sys.stderr)
        print(f"  Max context: {ready_event.max_context_tokens}", file=sys.stderr)
        print(f"  Max concurrent: {ready_event.max_concurrent_requests}", file=sys.stderr)
        print(f"  Vision: {ready_event.vision}\n", file=sys.stderr)
        
        # Send request
        print(f"Sending request (request_progress=True)...", file=sys.stderr)
        request_id = client.send_request(
            prompt_tokens=tuple(prompt_tokens),
            max_output_tokens=max_output_tokens,
            report_progress=True
        )
        
        print(f"Request ID: {request_id}\n", file=sys.stderr)
        
        # Receive events
        stats = client.receive_events(request_id)
        
        # Print results
        print("\n" + "="*70)
        print("BINARY PROTOCOL TEST RESULTS - prompt_2k.txt")
        print("="*70)
        print(f"Request ID:          {stats.request_id}")
        print(f"Finish reason:       {stats.finish_reason.name}")
        print(f"Prompt tokens:       {stats.prompt_tokens}")
        print(f"Completion tokens:   {stats.completion_tokens}")
        print(f"Matched prefix:      {stats.matched_prefix_tokens}")
        print()
        print(f"Prefill time:        {stats.prefill_seconds:.3f}s")
        print(f"Prefill throughput:  {stats.prefill_tok_s:.1f} tok/s")
        print()
        print(f"Decode time:         {stats.decode_seconds:.3f}s")
        print(f"Decode throughput:   {stats.decode_tok_s:.1f} tok/s")
        print()
        print(f"Total wall time:     {stats.wall_seconds:.3f}s")
        print()
        print(f"Generated tokens:    {len(stats.generated_tokens)}")
        print(f"Token IDs (first 20): {stats.generated_tokens[:20]}")
        
        # Decode text
        if stats.generated_tokens:
            decoded = tokenizer.decode(stats.generated_tokens)
            print(f"\nGenerated text (first 200 chars):")
            print(f"  {repr(decoded[:200])}")
        
        print("\n" + "="*70)
        print("✓ SUCCESS: Binary protocol client verified with 2K prompt")
        print("="*70)
        
        return 0
        
    except Exception as e:
        print(f"\n✗ ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1
    
    finally:
        print("\nShutting down engine...", file=sys.stderr)
        try:
            client.shutdown()
        except Exception as e:
            print(f"Shutdown error: {e}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
