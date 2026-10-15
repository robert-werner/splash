#!/usr/bin/env python3
"""Comprehensive benchmark comparing baseline vs all optimizations"""

import subprocess
import json
import time
import sys
from pathlib import Path

MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
PORT_BASE = 8080

def run_server(flags, port):
    """Start server with given flags"""
    cmd = [
        "./splash", "serve",
        "--model", MODEL,
        "--port", str(port),
        "--max-batch-size", "6",
    ] + flags
    
    print(f"\n🚀 Starting server: {' '.join(cmd)}")
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Wait for server to be ready
    time.sleep(20)
    
    return proc

def get_server_status(port):
    """Get server status via /status endpoint"""
    try:
        result = subprocess.run(
            ["curl", "-s", f"http://127.0.0.1:{port}/status"],
            capture_output=True,
            text=True,
            timeout=5
        )
        return json.loads(result.stdout)
    except Exception as e:
        print(f"❌ Failed to get status: {e}")
        return None

def benchmark_inference(port, prompt="Explain quantum computing", max_tokens=100):
    """Run inference benchmark"""
    payload = json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0.7
    })
    
    start = time.time()
    result = subprocess.run(
        ["curl", "-s", "-X", "POST",
         f"http://127.0.0.1:{port}/v1/chat/completions",
         "-H", "Content-Type: application/json",
         "-d", payload],
        capture_output=True,
        text=True,
        timeout=60
    )
    elapsed = time.time() - start
    
    try:
        response = json.loads(result.stdout)
        usage = response.get("usage", {})
        return {
            "success": True,
            "elapsed": elapsed,
            "prompt_tokens": usage.get("prompt_tokens", 0),
            "completion_tokens": usage.get("completion_tokens", 0),
            "total_tokens": usage.get("total_tokens", 0),
            "tokens_per_second": usage.get("completion_tokens", 0) / elapsed if elapsed > 0 else 0
        }
    except Exception as e:
        print(f"❌ Benchmark failed: {e}")
        return {"success": False, "error": str(e)}

def stop_server(proc):
    """Stop server process"""
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

def main():
    configurations = [
        {
            "name": "Baseline (no optimizations)",
            "flags": [],
            "port": PORT_BASE
        },
        {
            "name": "Phase 1 (Dynamic Batching)",
            "flags": ["--dynamic-batching"],
            "port": PORT_BASE + 1
        },
        {
            "name": "Phase 3 INT4 KV Cache",
            "flags": ["--int4-kv-cache"],
            "port": PORT_BASE + 2
        },
        {
            "name": "Phase 3 Vision Text-Only",
            "flags": ["--vision-text-only"],
            "port": PORT_BASE + 3
        },
        {
            "name": "All Optimizations",
            "flags": [
                "--dynamic-batching",
                "--int4-kv-cache",
                "--vision-text-only",
                "--kernel-fusion",
                "--speculative-decoding"
            ],
            "port": PORT_BASE + 4
        }
    ]
    
    results = []
    
    for config in configurations:
        print(f"\n{'='*80}")
        print(f"Testing: {config['name']}")
        print(f"{'='*80}")
        
        proc = run_server(config["flags"], config["port"])
        
        try:
            # Get status
            status = get_server_status(config["port"])
            
            if status:
                print(f"✅ Server ready")
                print(f"   Context capacity: {status.get('context_capacity', 'N/A')}")
                print(f"   Model: {status.get('model_id', 'N/A')}")
            
            # Run benchmark
            print(f"\n🔥 Running inference benchmark...")
            bench = benchmark_inference(config["port"])
            
            if bench["success"]:
                print(f"✅ Benchmark complete")
                print(f"   Tokens/second: {bench['tokens_per_second']:.2f}")
                print(f"   Total time: {bench['elapsed']:.2f}s")
                print(f"   Tokens: {bench['completion_tokens']}/{bench['total_tokens']}")
            
            results.append({
                "config": config["name"],
                "status": status,
                "benchmark": bench
            })
            
        finally:
            stop_server(proc)
            time.sleep(5)
    
    # Print summary
    print(f"\n{'='*80}")
    print("BENCHMARK SUMMARY")
    print(f"{'='*80}\n")
    
    print(f"{'Configuration':<40} {'Context':>12} {'Tok/s':>10} {'Time':>8}")
    print(f"{'-'*80}")
    
    for r in results:
        config = r["config"]
        status = r.get("status", {})
        bench = r.get("benchmark", {})
        
        context = status.get("context_capacity", "N/A") if status else "N/A"
        toks = f"{bench.get('tokens_per_second', 0):.2f}" if bench.get("success") else "FAIL"
        elapsed = f"{bench.get('elapsed', 0):.2f}s" if bench.get("success") else "N/A"
        
        print(f"{config:<40} {str(context):>12} {toks:>10} {elapsed:>8}")
    
    print(f"\n{'='*80}")
    
    # Save results to JSON
    with open("benchmark_results.json", "w") as f:
        json.dump(results, f, indent=2)
    
    print(f"\n✅ Results saved to benchmark_results.json")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
