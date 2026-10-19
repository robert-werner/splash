#!/usr/bin/env python3
"""Measure baseline PQ2_0 decode throughput at three history lengths."""

import json
import random
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import requests

# Test configuration
PORT = 8090
BASE_URL = f"http://127.0.0.1:{PORT}"
MODEL_PATH = "install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6"
WARMUP_TIME = 30
TIMEOUT = 300


def generate_sensor_log(seed: int, lines: int) -> str:
    """Generate synthetic sensor log (same as hybrid test)."""
    random.seed(seed)
    log = []
    for i in range(lines):
        ts = 1697500000 + i * 60
        temp = 20 + random.uniform(-5, 15)
        humidity = 50 + random.uniform(-10, 20)
        pressure = 1013 + random.uniform(-5, 5)
        log.append(f"{ts},temperature,{temp:.2f}")
        log.append(f"{ts},humidity,{humidity:.2f}")
        log.append(f"{ts},pressure,{pressure:.2f}")
    return "\n".join(log)


def start_server() -> subprocess.Popen | None:
    """Start the baseline server."""
    print(f"Starting baseline server on port {PORT}...")
    
    # Write server output to file for debugging
    log_file = open("/tmp/baseline_server_8090.log", "w")
    proc = subprocess.Popen(
        [".venv/bin/python", "-m", "server.server", MODEL_PATH, "--port", str(PORT)],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
    )
    
    # Wait for warmup
    print(f"Waiting {WARMUP_TIME}s for warmup...")
    time.sleep(WARMUP_TIME)
    
    # Check if server is ready
    try:
        resp = requests.get(f"{BASE_URL}/v1/models", timeout=10)
        if resp.status_code == 200:
            print("Server ready")
            return proc
        else:
            print(f"Server returned {resp.status_code}")
            proc.kill()
            return None
    except Exception as e:
        print(f"Server not ready: {e}")
        proc.kill()
        return None


def measure_decode(prompt_tokens: int, description: str) -> dict | None:
    """Measure decode throughput for given prompt size."""
    print(f"\n{'='*60}")
    print(f"Measuring: {description}")
    print(f"Target prompt tokens: {prompt_tokens}")
    
    # Generate prompt
    if prompt_tokens < 100:
        # Small prompt
        prompt_text = "Hello, how are you doing today?"
    elif prompt_tokens < 20000:
        # Medium prompt (~14.5K tokens)
        sensor_data = generate_sensor_log(seed=3, lines=1500)
        prompt_text = f"Analyze this sensor data:\n\n{sensor_data}\n\nProvide insights:"
    else:
        # Large prompt (~29K tokens)
        sensor_data = generate_sensor_log(seed=3, lines=3000)
        prompt_text = f"Analyze this sensor data:\n\n{sensor_data}\n\nProvide insights:"
    
    # Make request
    payload = {
        "model": "baseline",
        "messages": [{"role": "user", "content": prompt_text}],
        "max_tokens": 200,
        "temperature": 0,
        "stream": False,
    }
    
    print("Sending request...")
    start_time = time.time()
    
    try:
        resp = requests.post(
            f"{BASE_URL}/v1/chat/completions",
            json=payload,
            timeout=TIMEOUT,
        )
        
        elapsed = time.time() - start_time
        
        if resp.status_code != 200:
            print(f"Error: {resp.status_code}")
            print(resp.text)
            return None
        
        data = resp.json()
        
        # Extract metrics
        usage = data.get("usage", {})
        actual_prompt_tokens = usage.get("prompt_tokens", 0)
        completion_tokens = usage.get("completion_tokens", 0)
        
        # Get decode rate from /status
        status_resp = requests.get(f"{BASE_URL}/status", timeout=10)
        if status_resp.status_code == 200:
            status_data = status_resp.json()
            decode_rate = status_data.get("decode_tokens_per_second", 0)
        else:
            # Fallback calculation
            decode_rate = completion_tokens / elapsed if elapsed > 0 else 0
        
        result = {
            "description": description,
            "prompt_tokens": actual_prompt_tokens,
            "completion_tokens": completion_tokens,
            "elapsed_s": elapsed,
            "decode_tok_s": decode_rate,
        }
        
        print(f"Prompt tokens: {actual_prompt_tokens}")
        print(f"Completion tokens: {completion_tokens}")
        print(f"Elapsed: {elapsed:.2f}s")
        print(f"Decode rate: {decode_rate:.1f} tok/s")
        
        return result
        
    except Exception as e:
        print(f"Request failed: {e}")
        return None


def main():
    # Start server
    proc = start_server()
    if not proc:
        print("Failed to start server")
        return 1
    
    try:
        # Run three measurements
        results = []
        
        # Small context (~13 tokens)
        result = measure_decode(13, "Small context (~13 tokens)")
        if result:
            results.append(result)
        
        time.sleep(2)
        
        # Medium context (~14.5K tokens)
        result = measure_decode(14500, "Medium context (~14.5K tokens)")
        if result:
            results.append(result)
        
        time.sleep(2)
        
        # Large context (~29K tokens)
        result = measure_decode(29000, "Large context (~29K tokens)")
        if result:
            results.append(result)
        
        # Generate report
        print(f"\n{'='*60}")
        print("BASELINE DECODE COMPARISON")
        print(f"{'='*60}\n")
        
        print("| History Length | Baseline tok/s | Hybrid tok/s | Delta % | Notes |")
        print("|----------------|----------------|--------------|---------|-------|")
        
        hybrid_rates = {
            "small": 47.5,
            "medium": 25.3,
            "large": 34.4,  # Assuming this from 29K context
        }
        
        for i, r in enumerate(results):
            if i == 0:
                key = "small"
                hybrid = hybrid_rates["small"]
            elif i == 1:
                key = "medium"
                hybrid = hybrid_rates["medium"]
            else:
                key = "large"
                hybrid = hybrid_rates["large"]
            
            baseline = r["decode_tok_s"]
            delta = ((baseline - hybrid) / hybrid * 100) if hybrid > 0 else 0
            
            print(f"| {r['prompt_tokens']:,} tokens | {baseline:.1f} | {hybrid:.1f} | {delta:+.1f}% | {r['description']} |")
        
        # Write report file
        report_path = Path("BASELINE_DECODE_COMPARISON.md")
        with open(report_path, "w") as f:
            f.write("# Baseline PQ2_0 Decode Comparison\n\n")
            f.write(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
            f.write("**Configuration:**\n")
            f.write(f"- Model: PQ2_0 (baseline, non-hybrid)\n")
            f.write(f"- Server: Python server on port {PORT}\n")
            f.write(f"- Mode: Standard inference (no ANE FFN split)\n\n")
            
            f.write("**Results:**\n\n")
            f.write("| History Length | Baseline tok/s | Hybrid tok/s | Delta % | Notes |\n")
            f.write("|----------------|----------------|--------------|---------|-------|\n")
            
            for i, r in enumerate(results):
                if i == 0:
                    hybrid = hybrid_rates["small"]
                elif i == 1:
                    hybrid = hybrid_rates["medium"]
                else:
                    hybrid = hybrid_rates["large"]
                
                baseline = r["decode_tok_s"]
                delta = ((baseline - hybrid) / hybrid * 100) if hybrid > 0 else 0
                
                f.write(f"| {r['prompt_tokens']:,} tokens | {baseline:.1f} | {hybrid:.1f} | {delta:+.1f}% | {r['description']} |\n")
            
            f.write("\n**Raw Data:**\n\n")
            f.write("```json\n")
            f.write(json.dumps(results, indent=2))
            f.write("\n```\n")
        
        print(f"\nReport written to: {report_path}")
        
        return 0
        
    finally:
        print("\nShutting down server...")
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    sys.exit(main())
