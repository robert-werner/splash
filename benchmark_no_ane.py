#!/usr/bin/env python3
"""
NO ANE Prefill Benchmark — Control Group
Same prompts, GPU-only execution
"""

import requests
import time
import json
import sys
from typing import Dict, List

# Test configuration (SAME as ANE test)
PROMPT_SIZES = [1000, 5000, 10000]
BASE_URL = "http://127.0.0.1:8080"
TIMEOUT = 300

def generate_prompt(size: int) -> str:
    """Generate a prompt of approximately N tokens"""
    words_needed = int(size / 1.3)
    base_text = "The quick brown fox jumps over the lazy dog. "
    repeats = (words_needed // 9) + 1
    return base_text * repeats

def measure_ttft(prompt: str, size: int) -> Dict:
    """Measure Time To First Token (TTFT) = prefill time"""
    print(f"\n{'='*60}")
    print(f"Testing {size:,} token prompt...")
    print(f"{'='*60}")
    
    result: Dict = {"prompt_size": size, "success": False}
    
    payload = {
        "model": "unsloth/Qwen3.8-27B-GGUF:UD-IQ3_XXS",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 10,
        "temperature": 0.0,
        "stream": True
    }
    
    start_time = time.time()
    first_token_time = None
    token_count = 0
    
    try:
        response = requests.post(
            f"{BASE_URL}/v1/chat/completions",
            json=payload,
            stream=True,
            timeout=TIMEOUT
        )
        response.raise_for_status()
        
        for line in response.iter_lines():
            if not line or line == b'data: [DONE]':
                continue
                
            if line.startswith(b'data: '):
                try:
                    data = json.loads(line[6:])
                    if 'choices' in data and len(data['choices']) > 0:
                        delta = data['choices'][0].get('delta', {})
                        if 'content' in delta:
                            token_count += 1
                            if first_token_time is None:
                                first_token_time = time.time()
                                ttft = first_token_time - start_time
                                prefill_throughput = size / ttft
                                print(f"✅ TTFT: {ttft:.2f}s")
                                print(f"✅ Prefill: {prefill_throughput:.1f} tok/s")
                                
                                return {
                                    "prompt_size": size,
                                    "ttft": ttft,
                                    "prefill_throughput": prefill_throughput,
                                    "success": True
                                }
                except json.JSONDecodeError:
                    continue
        
        if first_token_time is None:
            print("❌ No tokens received")
            result["error"] = "no_tokens"
            return result
            
    except requests.exceptions.Timeout:
        print(f"❌ Timeout after {TIMEOUT}s")
        result["error"] = "timeout"
        return result
    except Exception as e:
        print(f"❌ Error: {e}")
        result["error"] = str(e)
        return result

def main():
    print("=" * 60)
    print("NO ANE BENCHMARK — GPU ONLY (Control)")
    print("=" * 60)
    
    # Check server status
    try:
        response = requests.get(f"{BASE_URL}/v1/models", timeout=10)
        response.raise_for_status()
        models = response.json()
        print(f"✅ Server ready: {BASE_URL}")
        print(f"✅ Models: {len(models.get('data', []))}")
    except Exception as e:
        print(f"❌ Server not ready: {e}")
        sys.exit(1)
    
    results = []
    
    for size in PROMPT_SIZES:
        prompt = generate_prompt(size)
        result = measure_ttft(prompt, size)
        results.append(result)
        time.sleep(2)
    
    # Summary
    print("\n" + "=" * 60)
    print("RESULTS SUMMARY — NO ANE (GPU ONLY)")
    print("=" * 60)
    print(f"{'Prompt Size':<15} {'TTFT (s)':<12} {'Prefill (tok/s)':<20}")
    print("-" * 60)
    
    for r in results:
        if r.get("success"):
            print(f"{r['prompt_size']:>10,} tok  {r['ttft']:>8.2f}     {r['prefill_throughput']:>12.1f}")
        else:
            print(f"{r['prompt_size']:>10,} tok  {'FAILED':<20} {r.get('error', 'unknown')}")
    
    # Save results
    output_file = "no_ane_benchmark_results.json"
    with open(output_file, "w") as f:
        json.dump({
            "model": "unsloth/Qwen3.8-27B-GGUF:UD-IQ3_XXS",
            "ane_enabled": False,
            "working_set": "22G",
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "results": results
        }, f, indent=2)
    
    print(f"\n✅ Results saved: {output_file}")
    
    # Load ANE results for comparison
    try:
        with open("ane_iq3_xxs_benchmark_results.json", "r") as f:
            ane_data = json.load(f)
            ane_results = ane_data["results"]
        
        print("\n" + "=" * 60)
        print("ANE vs NO ANE COMPARISON")
        print("=" * 60)
        print(f"{'Size':<10} {'ANE (tok/s)':<15} {'GPU (tok/s)':<15} {'Speedup':<10}")
        print("-" * 60)
        
        for ane_r, gpu_r in zip(ane_results, results):
            if ane_r.get("success") and gpu_r.get("success"):
                speedup = (ane_r["prefill_throughput"] / gpu_r["prefill_throughput"] - 1) * 100
                print(f"{ane_r['prompt_size']:>6,} tok  {ane_r['prefill_throughput']:>10.1f}     "
                      f"{gpu_r['prefill_throughput']:>10.1f}     {speedup:>+6.1f}%")
        
    except FileNotFoundError:
        print("\n⚠️  ANE results not found for comparison")

if __name__ == "__main__":
    main()
