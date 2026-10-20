#!/usr/bin/env python3
"""
Benchmark ANE prefill acceleration on hybrid models.

Compares prefill throughput:
1. Baseline (--ane off)
2. ANE enabled (--ane on)

Measures TTFT (time to first token) for various prompt sizes.
"""

import sys
sys.path.insert(0, '.')
from binary_protocol_client import BinaryProtocolClient
import time
import subprocess
import re

def measure_prefill(model_path: str, enable_ane: bool, prompt_tokens: int, runs: int = 3):
    """Measure prefill throughput (TTFT) for a given prompt size."""
    
    print(f"\n{'='*60}")
    print(f"  Model: {model_path.split('/')[-3]}")
    print(f"  ANE: {'ON' if enable_ane else 'OFF'}")
    print(f"  Prompt: {prompt_tokens:,} tokens")
    print('='*60)
    
    client = BinaryProtocolClient(
        engine_path="./build/splash",
        model_dir=model_path,
        context_size=prompt_tokens + 1000,
        memory="auto",
        ane="on" if enable_ane else "off"
    )
    
    try:
        start_launch = time.time()
        client.start()
        launch_time = time.time() - start_launch
        
        # Check for ANE availability in stderr
        stderr_output = ""
        if client.process and client.process.stderr:
            # Read available stderr without blocking
            import fcntl
            import os
            fd = client.process.stderr.fileno()
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            try:
                stderr_output = client.process.stderr.read().decode('utf-8', errors='replace')
            except:
                pass
        
        ane_active = "Neural Engine FFN split unavailable" not in stderr_output
        
        print(f"\n✅ Engine launched in {launch_time:.2f}s")
        print(f"   Max context: {client.ready_event.max_context_tokens:,} tokens")
        print(f"   ANE status: {'ACTIVE' if ane_active else 'UNAVAILABLE (rotated weights)'}")
        
        # Generate prompt of desired length (repeating pattern)
        # Token IDs: just repeat "The capital of France is " pattern
        base_tokens = [791, 6864, 315, 9822, 374]  # "The capital of France is"
        prompt = tuple(base_tokens * (prompt_tokens // len(base_tokens)))[:prompt_tokens]
        
        ttfts = []
        throughputs = []
        
        for run in range(runs):
            print(f"\n  Run {run+1}/{runs}...", end='', flush=True)
            
            start = time.time()
            request_id = client.send_request(
                prompt_tokens=prompt,
                max_output_tokens=1,  # Just first token
                report_progress=False
            )
            
            stats = client.receive_events(request_id)
            ttft = time.time() - start
            throughput = prompt_tokens / ttft
            
            ttfts.append(ttft)
            throughputs.append(throughput)
            
            print(f" TTFT={ttft:.2f}s, {throughput:.1f} tok/s")
        
        # Summary
        avg_ttft = sum(ttfts) / len(ttfts)
        avg_throughput = sum(throughputs) / len(throughputs)
        
        print(f"\n📊 Average: TTFT={avg_ttft:.2f}s, {avg_throughput:.1f} tok/s")
        
        return {
            'model': model_path.split('/')[-3],
            'ane': enable_ane,
            'ane_active': ane_active,
            'prompt_tokens': prompt_tokens,
            'ttft': avg_ttft,
            'throughput': avg_throughput,
            'runs': runs
        }
        
    finally:
        if hasattr(client, 'process') and client.process:
            client.process.terminate()
            try:
                client.process.wait(timeout=3)
            except:
                client.process.kill()

def main():
    print("""
╔══════════════════════════════════════════════════════════╗
║  ANE PREFILL ACCELERATION BENCHMARK                      ║
║  Comparing --ane on vs off on hybrid models              ║
╚══════════════════════════════════════════════════════════╝
""")
    
    # Test configurations
    models = [
        "install/models/ane-bench",
        "install/models/hybrid-real",
    ]
    
    prompt_sizes = [1000, 5000, 10000]  # Progressive sizes
    
    results = []
    
    for model_path in models:
        # Check if model exists
        import os
        if not os.path.exists(f"{model_path}/model.json"):
            print(f"⚠️  Skipping {model_path} (not found)")
            continue
        
        for prompt_size in prompt_sizes:
            # Baseline (ANE off)
            try:
                result_off = measure_prefill(model_path, enable_ane=False, prompt_tokens=prompt_size)
                results.append(result_off)
            except Exception as e:
                print(f"❌ Baseline failed: {e}")
                continue
            
            # ANE enabled
            try:
                result_on = measure_prefill(model_path, enable_ane=True, prompt_tokens=prompt_size)
                results.append(result_on)
            except Exception as e:
                print(f"❌ ANE test failed: {e}")
                continue
            
            # Compare
            if result_on['ane_active']:
                speedup = ((result_on['throughput'] - result_off['throughput']) / result_off['throughput']) * 100
                print(f"\n🎯 ANE Speedup: {speedup:+.1f}%")
                print(f"   {result_off['throughput']:.1f} tok/s → {result_on['throughput']:.1f} tok/s")
            else:
                print(f"\n⚠️  ANE unavailable (rotated weights), no speedup expected")
    
    # Final summary
    print(f"\n{'='*60}")
    print("  FINAL RESULTS")
    print('='*60)
    
    for result in results:
        ane_str = "ANE-ON" if result['ane'] else "ANE-OFF"
        active_str = "(active)" if result.get('ane_active') else "(inactive)"
        print(f"{result['model']:20s} {ane_str:8s} {active_str:10s} "
              f"{result['prompt_tokens']:6,} tok: {result['throughput']:6.1f} tok/s")
    
    # Calculate average speedup for active ANE
    ane_off_results = [r for r in results if not r['ane']]
    ane_on_results = [r for r in results if r['ane'] and r.get('ane_active')]
    
    if ane_on_results:
        avg_speedup = sum((on['throughput'] - off['throughput']) / off['throughput'] * 100 
                          for on, off in zip(ane_on_results, ane_off_results)) / len(ane_on_results)
        print(f"\n✅ Average ANE speedup: {avg_speedup:+.1f}%")
    else:
        print(f"\n⚠️  No active ANE results (all models have rotated weights)")

if __name__ == "__main__":
    main()
