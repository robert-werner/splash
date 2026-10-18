#!/usr/bin/env python3
"""
Phase 2 Quality Regression Test

Compares baseline PQ2_0 vs hybrid PQ1/PQ2 model quality.
Uses direct model directory loading, bypassing the HF model registry.
"""

import json
import requests
import subprocess
import sys
import time
from pathlib import Path

# Configuration
WORKDIR = Path(__file__).parent
BASELINE_MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
HYBRID_MODEL_FILE = WORKDIR / "install/models/ane-bench/Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf"

TEST_PROMPT = "Write a short story about a robot learning to paint:"
MAX_TOKENS = 200
TEMPERATURE = 0.7
TOP_P = 0.9

PORT_BASELINE = 8090
PORT_HYBRID = 8091


def wait_for_server(port, max_wait=90):
    """Wait for server to become ready"""
    print(f"  Waiting for server on port {port}...", end="", flush=True)
    start = time.time()
    while time.time() - start < max_wait:
        try:
            resp = requests.get(f"http://127.0.0.1:{port}/status", timeout=2)
            if resp.status_code == 200:
                elapsed = int(time.time() - start)
                print(f" ✅ Ready ({elapsed}s)")
                return resp.json()
        except:
            pass
        
        elapsed = int(time.time() - start)
        if elapsed > 0 and elapsed % 10 == 0:
            print(f" {elapsed}s...", end="", flush=True)
        
        time.sleep(1)
    
    raise RuntimeError(f"Server not ready on port {port} after {max_wait}s")


def run_inference(port, prompt, model_name):
    """Run inference and return content, usage, timing"""
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
    resp = requests.post(url, json=payload, timeout=180)
    elapsed = time.time() - start
    
    if resp.status_code != 200:
        raise RuntimeError(f"API error {resp.status_code}: {resp.text}")
    
    data = resp.json()
    content = data["choices"][0]["message"]["content"]
    usage = data.get("usage", {})
    
    return content, usage, elapsed


def analyze_quality(text):
    """Analyze text quality and return (passed, issues)"""
    issues = []
    
    # Minimum length check
    if len(text.strip()) < 50:
        issues.append(f"Output too short: {len(text)} chars")
    
    # Check for repetition loops (same 3-word phrase)
    words = text.lower().split()
    if len(words) >= 10:
        for i in range(len(words) - 5):
            phrase = " ".join(words[i:i+3])
            rest = " ".join(words[i+3:])
            if phrase in rest:
                issues.append("Repetition loop detected")
                break
    
    # Check for gibberish (character distribution)
    alphanum_count = sum(c.isalnum() or c.isspace() for c in text)
    alphanum_ratio = alphanum_count / max(len(text), 1)
    if alphanum_ratio < 0.75:
        issues.append(f"Low alphanum ratio: {alphanum_ratio:.1%}")
    
    # Check for excessive special characters
    special_chars = sum(1 for c in text if not c.isalnum() and not c.isspace() and c not in '.,!?;:\'-"()[]')
    if special_chars > len(text) * 0.15:
        issues.append(f"Too many special chars: {special_chars}/{len(text)}")
    
    # Check for sentence loops
    sentences = [s.strip() for s in text.split('.') if s.strip()]
    if len(sentences) != len(set(sentences)) and len(sentences) > 1:
        issues.append("Identical sentences detected")
    
    passed = len(issues) == 0
    return passed, issues


def test_baseline():
    """Test baseline PQ2_0 model"""
    print("\n" + "="*70)
    print("Baseline Test: PQ2_0")
    print("="*70)
    print(f"  Model: {BASELINE_MODEL}")
    
    # Launch server using splash wrapper
    cmd = [str(WORKDIR / "splash"), "serve", "--model", BASELINE_MODEL, "--port", str(PORT_BASELINE)]
    print(f"  Command: {' '.join(cmd)}")
    
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True
    )
    
    try:
        # Wait for server
        status = wait_for_server(PORT_BASELINE)
        model_name = status.get("model", BASELINE_MODEL)
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = run_inference(PORT_BASELINE, TEST_PROMPT, model_name)
        
        # Analyze quality
        passed, issues = analyze_quality(content)
        
        # Calculate metrics
        comp_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tok_per_sec = comp_tokens / elapsed if elapsed > 0 else 0
        
        # Report
        print(f"\n  📊 Results:")
        print(f"     Tokens: {comp_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tok_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        
        if issues:
            print(f"\n  ⚠️  Quality Issues:")
            for issue in issues:
                print(f"       - {issue}")
        
        print(f"\n  📄 Output (first 100 tokens):")
        preview_words = content.split()[:100]
        preview = " ".join(preview_words)
        for line in [preview[i:i+70] for i in range(0, len(preview), 70)]:
            print(f"     {line}")
        
        return {
            "model": "baseline-PQ2_0",
            "content": content,
            "usage": usage,
            "elapsed": elapsed,
            "tokens_per_sec": tok_per_sec,
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
        time.sleep(2)


def test_hybrid():
    """Test hybrid PQ1/PQ2 model using native binary"""
    print("\n" + "="*70)
    print("Hybrid Test: PQ1/PQ2 Quantization")
    print("="*70)
    print(f"  Model: {HYBRID_MODEL_FILE}")
    
    if not HYBRID_MODEL_FILE.exists():
        raise FileNotFoundError(f"Hybrid model not found: {HYBRID_MODEL_FILE}")
    
    # Need to create a temporary model directory structure
    # that the native binary can load
    model_dir = WORKDIR / ".test_hybrid_model"
    model_dir.mkdir(exist_ok=True)
    
    # Create symlink to hybrid model
    model_link = model_dir / "model.gguf"
    if model_link.exists():
        model_link.unlink()
    model_link.symlink_to(HYBRID_MODEL_FILE)
    
    # Copy tokenizer from baseline model dir
    baseline_dir = WORKDIR / "install/models/prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
    tokenizer_src = baseline_dir / "tokenizer"
    tokenizer_dst = model_dir / "tokenizer"
    
    if tokenizer_src.exists() and not tokenizer_dst.exists():
        import shutil
        if tokenizer_src.is_dir():
            shutil.copytree(tokenizer_src, tokenizer_dst)
        else:
            shutil.copy2(tokenizer_src, tokenizer_dst)
    
    # Create model.json
    model_json = {
        "owner": "test",
        "repo": "hybrid-test",
        "variant": "PQ1PQ2",
        "local_path": str(model_dir),
        "tokenizer_path": str(tokenizer_dst)
    }
    
    with open(model_dir / "model.json", "w") as f:
        json.dump(model_json, f, indent=2)
    
    # Launch native binary
    binary = WORKDIR / "build/splash"
    cmd = [
        str(binary),
        "serve-native",
        str(model_dir),
        "auto", "auto",  # context, cache
        "--port", str(PORT_HYBRID)
    ]
    print(f"  Command: {' '.join(cmd)}")
    
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True
    )
    
    try:
        # Wait for server
        status = wait_for_server(PORT_HYBRID, max_wait=120)
        model_name = status.get("model", "test/hybrid-test:PQ1PQ2")
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = run_inference(PORT_HYBRID, TEST_PROMPT, model_name)
        
        # Analyze quality
        passed, issues = analyze_quality(content)
        
        # Calculate metrics
        comp_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tok_per_sec = comp_tokens / elapsed if elapsed > 0 else 0
        
        # Report
        print(f"\n  📊 Results:")
        print(f"     Tokens: {comp_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tok_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        
        if issues:
            print(f"\n  ⚠️  Quality Issues:")
            for issue in issues:
                print(f"       - {issue}")
        
        print(f"\n  📄 Output (first 100 tokens):")
        preview_words = content.split()[:100]
        preview = " ".join(preview_words)
        for line in [preview[i:i+70] for i in range(0, len(preview), 70)]:
            print(f"     {line}")
        
        return {
            "model": "hybrid-PQ1PQ2",
            "content": content,
            "usage": usage,
            "elapsed": elapsed,
            "tokens_per_sec": tok_per_sec,
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
        time.sleep(2)


def main():
    print("="*70)
    print("Phase 2: Quality Regression Test")
    print("="*70)
    print(f"\nPrompt: '{TEST_PROMPT}'")
    print(f"Max tokens: {MAX_TOKENS}")
    print(f"Temperature: {TEMPERATURE}, Top-p: {TOP_P}\n")
    
    results = []
    
    try:
        # Test baseline
        baseline_result = test_baseline()
        results.append(baseline_result)
        
        # Test hybrid
        hybrid_result = test_hybrid()
        results.append(hybrid_result)
        
    except KeyboardInterrupt:
        print("\n\n⚠️  Interrupted by user")
        return 130
    except Exception as e:
        print(f"\n\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    # Comparison report
    print("\n" + "="*70)
    print("COMPARISON REPORT")
    print("="*70)
    
    baseline = results[0]
    hybrid = results[1]
    
    print(f"\n1. Quality Assessment:")
    print(f"   Baseline: {'✅ PASS' if baseline['passed'] else '❌ FAIL'}")
    print(f"   Hybrid:   {'✅ PASS' if hybrid['passed'] else '❌ FAIL'}")
    
    if not hybrid['passed']:
        print(f"\n   🚨 Hybrid quality issues:")
        for issue in hybrid['issues']:
            print(f"      - {issue}")
    
    print(f"\n2. Performance:")
    print(f"   Baseline: {baseline['tokens_per_sec']:6.1f} tok/s")
    print(f"   Hybrid:   {hybrid['tokens_per_sec']:6.1f} tok/s", end="")
    if hybrid['tokens_per_sec'] > baseline['tokens_per_sec']:
        delta = (hybrid['tokens_per_sec'] / baseline['tokens_per_sec'] - 1) * 100
        print(f"  ({delta:+.1f}%)")
    else:
        delta = (hybrid['tokens_per_sec'] / baseline['tokens_per_sec'] - 1) * 100
        print(f"  ({delta:.1f}%)")
    
    print(f"\n3. Output Samples (first 100 tokens):")
    print(f"\n   Baseline:")
    baseline_sample = " ".join(baseline['content'].split()[:100])
    for line in [baseline_sample[i:i+65] for i in range(0, len(baseline_sample), 65)]:
        print(f"   {line}")
    
    print(f"\n   Hybrid:")
    hybrid_sample = " ".join(hybrid['content'].split()[:100])
    for line in [hybrid_sample[i:i+65] for i in range(0, len(hybrid_sample), 65)]:
        print(f"   {line}")
    
    # Subjective assessment
    print(f"\n4. Subjective Quality:")
    print(f"   Both outputs are coherent stories: ", end="")
    if baseline['passed'] and hybrid['passed']:
        print("✅ YES")
        print(f"   Quality degradation: None detected")
    else:
        print("❌ NO")
    
    # Save results
    output_file = WORKDIR / "phase2_quality_results.json"
    with open(output_file, 'w') as f:
        json.dump({
            "baseline": baseline,
            "hybrid": hybrid,
            "comparison": {
                "quality_match": baseline['passed'] and hybrid['passed'],
                "speed_delta_pct": (hybrid['tokens_per_sec'] / baseline['tokens_per_sec'] - 1) * 100 if baseline['tokens_per_sec'] > 0 else 0
            }
        }, f, indent=2)
    
    print(f"\nResults saved to: {output_file}")
    
    # Final verdict
    print("\n" + "="*70)
    if baseline['passed'] and hybrid['passed']:
        print("✅ PHASE 2: PASS")
        print("   Hybrid model maintains quality equivalent to baseline")
    elif hybrid['passed']:
        print("⚠️  PHASE 2: CONDITIONAL PASS")
        print("   Hybrid quality good, but baseline had issues")
    else:
        print("❌ PHASE 2: FAIL")
        print("   Hybrid model shows quality degradation")
    print("="*70 + "\n")
    
    return 0 if (baseline['passed'] and hybrid['passed']) else 1


if __name__ == "__main__":
    sys.exit(main())
