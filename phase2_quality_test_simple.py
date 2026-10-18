#!/usr/bin/env python3
"""
Phase 2 Quality Regression Test

Compares hybrid model output quality against baseline PQ2_0.
Uses the 'splash' wrapper to properly handle tokenizers and model loading.
"""

import subprocess
import json
import time
import sys
from pathlib import Path

WORKDIR = Path(__file__).parent
BASELINE_MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
HYBRID_MODEL = "prism-ml/ane-bench-hybrid:PQ1PQ2"  # Uses the install/models/ane-bench-hybrid directory via symlink

TEST_PROMPT = "Write a short story about a robot learning to paint:"
MAX_TOKENS = 200
PORT_BASELINE = 8090
PORT_HYBRID = 8091

def wait_for_server(port, max_wait=90):
    """Wait for server to be ready"""
    print(f"  Waiting for server on port {port}...", end="", flush=True)
    start = time.time()
    while time.time() - start < max_wait:
        try:
            result = subprocess.run(
                ["curl", "-s", f"http://127.0.0.1:{port}/status"],
                capture_output=True,
                timeout=3
            )
            if result.returncode == 0 and result.stdout:
                elapsed = int(time.time() - start)
                print(f" ✅ Ready ({elapsed}s)")
                return json.loads(result.stdout)
        except:
            pass
        
        elapsed = int(time.time() - start)
        if elapsed > 0 and elapsed % 10 == 0:
            print(f" {elapsed}s...", end="", flush=True)
        
        time.sleep(0.5)
    
    raise RuntimeError(f"Server on port {port} not ready after {max_wait}s")

def run_inference(port, prompt, model_name):
    """Run inference and return (text, usage, elapsed_time)"""
    url = f"http://127.0.0.1:{port}/v1/chat/completions"
    
    payload = {
        "model": model_name,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": MAX_TOKENS,
        "temperature": 0.7,
        "top_p": 0.9,
        "stream": False
    }
    
    start = time.time()
    result = subprocess.run(
        [
            "curl", "-s", "-X", "POST", url,
            "-H", "Content-Type: application/json",
            "-d", json.dumps(payload)
        ],
        capture_output=True,
        text=True,
        timeout=180
    )
    elapsed = time.time() - start
    
    if result.returncode != 0:
        raise RuntimeError(f"curl failed: {result.stderr}")
    
    data = json.loads(result.stdout)
    if "error" in data:
        raise RuntimeError(f"API error: {data['error']}")
    
    content = data["choices"][0]["message"]["content"]
    usage = data.get("usage", {})
    
    return content, usage, elapsed

def analyze_quality(text):
    """Check for quality issues"""
    issues = []
    
    # Minimum length
    if len(text.strip()) < 50:
        issues.append(f"Too short: {len(text)} chars")
    
    # Word repetition (same word 3+ times in a row)
    words = text.lower().split()
    for i in range(len(words) - 2):
        if words[i] == words[i+1] == words[i+2]:
            issues.append(f"Word loop: '{words[i]}' x3")
            break
    
    # Sentence repetition
    sentences = [s.strip() for s in text.split('.') if s.strip()]
    if len(sentences) >= 2:
        for i in range(len(sentences) - 1):
            if sentences[i] and sentences[i] == sentences[i+1]:
                issues.append(f"Sentence repetition")
                break
    
    # Alphanumeric ratio (detect gibberish)
    alphanum_ratio = sum(c.isalnum() or c.isspace() for c in text) / max(len(text), 1)
    if alphanum_ratio < 0.80:
        issues.append(f"Low alphanumeric ratio: {alphanum_ratio:.1%}")
    
    # Excessive special characters
    special_chars = sum(1 for c in text if not c.isalnum() and not c.isspace() and c not in '.,!?;:\'"-()[]')
    if special_chars > len(text) * 0.15:
        issues.append(f"Too many special chars: {special_chars}/{len(text)}")
    
    return len(issues) == 0, issues

def test_model(name, model_spec, port):
    """Test one model configuration"""
    print(f"\n{'='*70}")
    print(f"{name}")
    print(f"{'='*70}")
    print(f"  Model: {model_spec}")
    
    # Start server using splash wrapper
    cmd = [str(WORKDIR / "splash"), "serve", "--model", model_spec, "--port", str(port)]
    print(f"  Command: {' '.join(cmd)}")
    
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    
    try:
        # Wait for ready
        status = wait_for_server(port)
        model_name = status.get("model", model_spec)
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = run_inference(port, TEST_PROMPT, model_name)
        
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
                print(f"     - {issue}")
        
        print(f"\n  📄 Output (first 100 tokens):")
        first_100 = ' '.join(content.split()[:100])
        for line in [first_100[i:i+70] for i in range(0, len(first_100), 70)]:
            print(f"     {line}")
        
        return {
            "name": name,
            "model": model_spec,
            "output": content,
            "usage": usage,
            "elapsed": elapsed,
            "tok_per_sec": tok_per_sec,
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
        time.sleep(2)  # Let port release

def main():
    print("="*70)
    print("Phase 2: Quality Regression Test")
    print("="*70)
    print(f"\nTest prompt: '{TEST_PROMPT}'")
    print(f"Max tokens: {MAX_TOKENS}\n")
    
    # Check that splash wrapper exists
    if not (WORKDIR / "splash").exists():
        print("❌ Error: splash wrapper not found")
        return 1
    
    results = []
    
    try:
        # Test baseline
        results.append(test_model(
            "Baseline: Pure PQ2_0",
            BASELINE_MODEL,
            PORT_BASELINE
        ))
        
        # Test hybrid
        results.append(test_model(
            "Hybrid: PQ1/PQ2 Quantization",
            HYBRID_MODEL,
            PORT_HYBRID
        ))
        
    except KeyboardInterrupt:
        print("\n\n⚠️  Test interrupted")
        return 130
    except Exception as e:
        print(f"\n\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    # Comparison
    print(f"\n{'='*70}")
    print("COMPARISON")
    print(f"{'='*70}")
    
    baseline = results[0]
    hybrid = results[1]
    
    print(f"\n1. Quality:")
    print(f"   Baseline: {'✅ PASS' if baseline['passed'] else '❌ FAIL'}")
    print(f"   Hybrid:   {'✅ PASS' if hybrid['passed'] else '❌ FAIL'}")
    
    if not hybrid['passed']:
        print(f"\n   🚨 Hybrid quality issues:")
        for issue in hybrid['issues']:
            print(f"      - {issue}")
    
    print(f"\n2. Performance:")
    print(f"   Baseline: {baseline['tok_per_sec']:6.1f} tok/s")
    print(f"   Hybrid:   {hybrid['tok_per_sec']:6.1f} tok/s  ", end="")
    if hybrid['tok_per_sec'] > baseline['tok_per_sec']:
        delta = (hybrid['tok_per_sec'] / baseline['tok_per_sec'] - 1) * 100
        print(f"({delta:+.1f}%)")
    else:
        print()
    
    print(f"\n3. Sample Outputs (first 100 tokens):")
    print(f"\n   Baseline:")
    baseline_sample = ' '.join(baseline['output'].split()[:100])
    for line in [baseline_sample[i:i+65] for i in range(0, len(baseline_sample), 65)]:
        print(f"   {line}")
    
    print(f"\n   Hybrid:")
    hybrid_sample = ' '.join(hybrid['output'].split()[:100])
    for line in [hybrid_sample[i:i+65] for i in range(0, len(hybrid_sample), 65)]:
        print(f"   {line}")
    
    # Final verdict
    print(f"\n{'='*70}")
    if hybrid['passed']:
        print("✅ PHASE 2: PASS")
        print("   Hybrid model maintains quality")
        if hybrid['tok_per_sec'] > baseline['tok_per_sec']:
            speedup = (hybrid['tok_per_sec'] / baseline['tok_per_sec'] - 1) * 100
            print(f"   Performance improvement: +{speedup:.1f}%")
    else:
        print("❌ PHASE 2: FAIL")
        print("   Hybrid model shows quality degradation")
    print(f"{'='*70}\n")
    
    # Save results
    output_file = WORKDIR / "phase2_quality_results.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"Results saved to: {output_file}")
    
    return 0 if hybrid['passed'] else 1

if __name__ == "__main__":
    sys.exit(main())
