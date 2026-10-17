#!/usr/bin/env python3
"""Quick test to verify priority scheduling works."""

import json
import requests
import time
from statistics import mean, median

API_BASE = "http://localhost:8081/v1"

def send_completion(prompt: str, max_tokens: int = 50) -> dict:
    """Send a completion request and return timing info."""
    start = time.time()
    
    response = requests.post(
        f"{API_BASE}/completions",
        json={
            "model": "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0",
            "prompt": prompt,
            "max_tokens": max_tokens,
            "temperature": 0.0,
        },
        timeout=300,
    )
    
    end = time.time()
    
    if response.status_code != 200:
        print(f"Error: {response.status_code} - {response.text}")
        return None
    
    result = response.json()
    ttft = None
    if "usage" in result and "time_to_first_token" in result["usage"]:
        ttft = result["usage"]["time_to_first_token"]
    
    return {
        "total_time": end - start,
        "ttft": ttft,
        "prompt_tokens": len(prompt.split()),
        "text": result.get("choices", [{}])[0].get("text", "")[:50],
    }

def main():
    print("Testing priority scheduling...")
    print("=" * 60)
    
    # Short prompt (interactive)
    short_prompt = "Hello, how are you today?"
    
    # Long prompt (batch)
    long_prompt = " ".join([f"Word{i}" for i in range(5000)])
    
    print("\n1. Short request alone (baseline):")
    result = send_completion(short_prompt)
    if result:
        print(f"   TTFT: {result['ttft']:.2f}s, Total: {result['total_time']:.2f}s")
    
    print("\n2. Long request alone (baseline):")
    result = send_completion(long_prompt)
    if result:
        print(f"   TTFT: {result['ttft']:.2f}s, Total: {result['total_time']:.2f}s")
    
    print("\n3. Send long request, then short (priority test):")
    print("   Sending long request in background...")
    # TODO: implement concurrent test
    
    print("\nTest complete. Run full benchmark with measure_mixed_latency.py")

if __name__ == "__main__":
    main()
