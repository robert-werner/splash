#!/usr/bin/env python3
"""Quick benchmark comparison: baseline vs all optimizations"""

import subprocess
import json
import time
import sys

MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"

def run_test(name, port, flags):
    """Run server and benchmark"""
    print(f"\n{'='*80}")
    print(f"Testing: {name}")
    print(f"{'='*80}\n")
    
    cmd = ["./splash", "serve", "--model", MODEL, "--port", str(port), "--max-batch-size", "6"] + flags
    print(f"Command: {' '.join(cmd)}\n")
    
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(25)
    
    try:
        # Get status
        status_result = subprocess.run(
            ["curl", "-s", f"http://127.0.0.1:{port}/status"],
            capture_output=True, text=True, timeout=5
        )
        status = json.loads(status_result.stdout)
        
        print(f"✅ Status:")
        print(f"   Context capacity: {status.get('context_capacity', 'N/A')} tokens")
        print(f"   Model: {status.get('model_id', 'N/A')}")
        print(f"   KV pages: {status.get('kv_pages', 'N/A')}")
        
        # Quick inference
        payload = json.dumps({
            "model": MODEL,
            "messages": [{"role": "user", "content": "Count to 5"}],
            "max_tokens": 50,
            "temperature": 0.7
        })
        
        start = time.time()
        inf_result = subprocess.run(
            ["curl", "-s", "-X", "POST", f"http://127.0.0.1:{port}/v1/chat/completions",
             "-H", "Content-Type: application/json", "-d", payload],
            capture_output=True, text=True, timeout=60
        )
        elapsed = time.time() - start
        
        response = json.loads(inf_result.stdout)
        usage = response.get("usage", {})
        
        print(f"\n✅ Inference:")
        print(f"   Tokens/s: {usage.get('completion_tokens', 0) / elapsed:.2f}")
        print(f"   Time: {elapsed:.2f}s")
        print(f"   Tokens: {usage.get('completion_tokens', 0)}/{usage.get('total_tokens', 0)}")
        
        return {
            "name": name,
            "context_capacity": status.get("context_capacity"),
            "tokens_per_second": usage.get("completion_tokens", 0) / elapsed,
            "latency": elapsed
        }
        
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        time.sleep(5)

def main():
    results = []
    
    # Baseline
    results.append(run_test("Baseline", 8080, []))
    
    # Phase 1 Dynamic Batching
    results.append(run_test("Dynamic Batching", 8081, ["--dynamic-batching"]))
    
    # Phase 3 INT4
    results.append(run_test("INT4 KV Cache", 8082, ["--int4-kv-cache"]))
    
    # All combined
    results.append(run_test("All Optimizations", 8083, [
        "--dynamic-batching", "--int4-kv-cache"
    ]))
    
    print(f"\n{'='*80}")
    print("SUMMARY")
    print(f"{'='*80}\n")
    print(f"{'Configuration':<30} {'Context':>12} {'Tok/s':>10} {'Latency':>10}")
    print(f"{'-'*80}")
    
    for r in results:
        print(f"{r['name']:<30} {r['context_capacity']:>12} "
              f"{r['tokens_per_second']:>10.2f} {r['latency']:>10.2f}s")
    
    with open("benchmark_quick_results.json", "w") as f:
        json.dump(results, f, indent=2)
    
    print(f"\n✅ Results saved to benchmark_quick_results.json\n")

if __name__ == "__main__":
    sys.exit(main())
