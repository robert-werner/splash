#!/usr/bin/env python3
"""
Quality regression test for hybrid quantization.
Tests that hybrid PQ1/PQ2 model produces coherent output.
"""

import json
import requests
import subprocess
import sys
import time
from pathlib import Path

WORKDIR = Path(__file__).parent
BASELINE_MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
HYBRID_MODEL_DIR = WORKDIR / "install/models/hybrid-test"
BINARY = WORKDIR / "build/splash"

TEST_PROMPT = "Explain the theory of relativity in simple terms."
MAX_TOKENS = 200
TEMPERATURE = 0.7
TOP_P = 0.9

def wait_for_server(port, max_wait=90):
    """Wait for server to be ready"""
    print(f"  Waiting for server on port {port}...", end="", flush=True)
    start = time.time()
    while time.time() - start < max_wait:
        try:
            resp = requests.get(f"http://127.0.0.1:{port}/status", timeout=1)
            if resp.status_code == 200:
                elapsed = int(time.time() - start)
                print(f" ✅ Ready ({elapsed}s)")
                return resp.json()
        except:
            pass
        
        # Progress indicator
        elapsed = int(time.time() - start)
        if elapsed > 0 and elapsed % 10 == 0:
            print(f" {elapsed}s...", end="", flush=True)
        
        time.sleep(0.5)
    
    raise RuntimeError(f"Server on port {port} not ready after {max_wait}s")

def run_inference(port, prompt, model_name):
    """Run inference and return generated text, usage, timing"""
    url = f"http://127.0.0.1:{port}/v1/chat/completions"
    
    payload = {
        "model": model_name,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": MAX_TOKENS,
        "temperature": TEMPERATURE,
        "top_p": TOP_P,
        "stream": False
    }
    
    start = time.time()
    resp = requests.post(url, json=payload, timeout=120)
    elapsed = time.time() - start
    
    if resp.status_code != 200:
        raise RuntimeError(f"API error {resp.status_code}: {resp.text}")
    
    data = resp.json()
    content = data["choices"][0]["message"]["content"]
    usage = data.get("usage", {})
    
    return content, usage, elapsed

def analyze_quality(text):
    """Check for quality issues"""
    issues = []
    
    # Check for repetition (same 10-char substring repeated)
    words = text.split()
    for i in range(len(words) - 3):
        chunk = " ".join(words[i:i+3])
        rest = " ".join(words[i+3:])
        if chunk in rest:
            issues.append("Repetition detected")
            break
    
    # Check for gibberish (too many non-ASCII or special chars)
    non_ascii = sum(1 for c in text if ord(c) > 127)
    if non_ascii / len(text) > 0.3:
        issues.append("High non-ASCII ratio")
    
    # Check coherence (at least some recognizable words)
    word_like = len([w for w in words if len(w) > 2 and w.isalpha()])
    if word_like < len(words) * 0.5:
        issues.append("Low word ratio - possible gibberish")
    
    # Check for loops (exact sentence repetition)
    sentences = [s.strip() for s in text.split('.') if s.strip()]
    if len(sentences) != len(set(sentences)):
        issues.append("Sentence loop detected")
    
    return len(issues) == 0, issues

def test_baseline():
    """Test baseline PQ2_0 model"""
    print("\n" + "="*70)
    print("Baseline: Pure PQ2_0")
    print("="*70)
    print(f"  Model: {BASELINE_MODEL}")
    
    # Launch server
    cmd = [str(WORKDIR / "splash"), "serve", "--model", BASELINE_MODEL, "--port", "8090"]
    print(f"  Command: {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    
    try:
        # Wait for ready
        status = wait_for_server(8090)
        model_name = status.get("model", BASELINE_MODEL)
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = run_inference(8090, TEST_PROMPT, model_name)
        
        # Analyze
        passed, issues = analyze_quality(content)
        
        # Report
        print(f"\n  📊 Results:")
        comp_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tok_per_sec = comp_tokens / elapsed if elapsed > 0 else 0
        print(f"     Tokens: {comp_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tok_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        if issues:
            for issue in issues:
                print(f"       - {issue}")
        
        print(f"\n  📄 Output preview (first 300 chars):")
        print(f"     {content[:300]}")
        
        return {
            "model": BASELINE_MODEL,
            "content": content,
            "usage": usage,
            "elapsed": elapsed,
            "passed": passed,
            "issues": issues
        }
    
    finally:
        print(f"  Stopping server...")
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

def test_hybrid(ane_layers=None):
    """Test hybrid quantization model"""
    title = "Hybrid Quantization"
    if ane_layers:
        title += f" (ANE layers: {ane_layers})"
    else:
        title += " (no ANE)"
    
    print("\n" + "="*70)
    print(title)
    print("="*70)
    print(f"  Model dir: {HYBRID_MODEL_DIR}")
    
    # Build command using server.py module directly
    # Need to use the same Python that has the server module available
    import os
    
    # Use miniforge3 python which has transformers installed
    python_cmd = "/Users/a1111/miniforge3/bin/python3"
    
    cmd = [
        python_cmd, "-u", "-P", "-m", "server.server",
        str(HYBRID_MODEL_DIR),
        "--tokenizer", str(HYBRID_MODEL_DIR / "tokenizer"),
        "--model", "prism-ml/hybrid-test",  # Must be in owner/repo format
        "--binary", str(BINARY),
        "--port", "8091",
        "--kv-format=int4",
        "--vision-text-only",
        "--kv-swap-enable=on",
        "--kv-swap-hot-tier=200000",
        "--enable-weight-prefetch=on"
    ]
    
    if ane_layers:
        cmd.extend(["--ane-layers", ane_layers])
    
    print(f"  Command: {' '.join(cmd)}")
    
    # Set environment
    env = os.environ.copy()
    env["PYTHONPATH"] = str(WORKDIR)
    env["PYTHONUNBUFFERED"] = "1"
    env["TRANSFORMERS_VERBOSITY"] = "error"
    
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=str(WORKDIR))
    
    try:
        # Wait for ready
        status = wait_for_server(8091, max_wait=120)
        model_name = status.get("model", "hybrid-test")
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = run_inference(8091, TEST_PROMPT, model_name)
        
        # Analyze
        passed, issues = analyze_quality(content)
        
        # Report
        print(f"\n  📊 Results:")
        comp_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tok_per_sec = comp_tokens / elapsed if elapsed > 0 else 0
        print(f"     Tokens: {comp_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tok_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        if issues:
            for issue in issues:
                print(f"       - {issue}")
        
        print(f"\n  📄 Output preview (first 300 chars):")
        print(f"     {content[:300]}")
        
        return {
            "model": "hybrid-test",
            "ane_layers": ane_layers,
            "content": content,
            "usage": usage,
            "elapsed": elapsed,
            "passed": passed,
            "issues": issues
        }
    
    finally:
        print(f"  Stopping server...")
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

def main():
    print("="*70)
    print("Quality Regression Test: Hybrid GGUF with ANE Layers")
    print("="*70)
    print(f"\nTest prompt: '{TEST_PROMPT}'")
    print(f"Max tokens: {MAX_TOKENS}")
    print(f"Temperature: {TEMPERATURE}, Top-p: {TOP_P}")
    
    results = []
    
    try:
        # Test baseline
        results.append(test_baseline())
        
        # Test hybrid without ANE
        results.append(test_hybrid())
        
        # Test hybrid with ANE layers
        results.append(test_hybrid(ane_layers="0,2,4,8,10,14"))
        
    except Exception as e:
        print(f"\n\n❌ Test failed with error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    # Summary
    print("\n\n" + "="*70)
    print("SUMMARY")
    print("="*70)
    
    for r in results:
        model = r.get("model", "?")
        ane = r.get("ane_layers")
        if ane:
            model += f" (ANE: {ane})"
        passed = "✅ PASS" if r["passed"] else "❌ FAIL"
        print(f"\n{model}: {passed}")
        if r["issues"]:
            for issue in r["issues"]:
                print(f"  - {issue}")
    
    # Compare quality
    print("\n" + "="*70)
    print("QUALITY COMPARISON")
    print("="*70)
    
    baseline = results[0]
    hybrid_no_ane = results[1]
    hybrid_ane = results[2]
    
    print(f"\nBaseline output (first 100 tokens):")
    baseline_words = baseline["content"].split()[:100]
    print(" ".join(baseline_words))
    
    print(f"\n\nHybrid (no ANE) output (first 100 tokens):")
    hybrid_no_ane_words = hybrid_no_ane["content"].split()[:100]
    print(" ".join(hybrid_no_ane_words))
    
    print(f"\n\nHybrid (ANE) output (first 100 tokens):")
    hybrid_ane_words = hybrid_ane["content"].split()[:100]
    print(" ".join(hybrid_ane_words))
    
    # Final verdict
    all_passed = all(r["passed"] for r in results)
    print("\n" + "="*70)
    if all_passed:
        print("✅ ALL TESTS PASSED - Hybrid quantization quality verified")
    else:
        print("❌ SOME TESTS FAILED - Quality regression detected")
    print("="*70)
    
    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())
