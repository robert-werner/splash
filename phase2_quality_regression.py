#!/usr/bin/env python3
"""
Phase 2 Quality Regression Test
Tests hybrid model vs baseline for coherent generation quality.

Starts servers, generates 200 tokens, compares output quality.
"""

import subprocess
import json
import os
import sys
import time
import signal
from pathlib import Path

WORKDIR = Path(__file__).parent
BINARY = WORKDIR / "splash"

# Models  
BASELINE_MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
HYBRID_MODEL_DIR = WORKDIR / "install/models/ane-bench"
HYBRID_GGUF = HYBRID_MODEL_DIR / "Ternary-Bonsai-2-27B-hybrid-ptq1pq2-fixed.gguf"

# Test parameters
TEST_PROMPT = "Write a short story about a robot learning to paint:"
MAX_TOKENS = 200
TEMPERATURE = 0.7
TOP_P = 0.9

# Ports
PORT_BASELINE = 8090
PORT_HYBRID = 8091


class ServerRunner:
    def __init__(self):
        self.procs = []
    
    def __enter__(self):
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Clean up all server processes"""
        for proc in self.procs:
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            except Exception:
                pass
    
    def wait_for_server(self, port: int, max_wait: int = 90) -> dict:
        """Wait for server to become ready"""
        print(f"  Waiting for server on port {port}...", end="", flush=True)
        
        for i in range(max_wait):
            time.sleep(1)
            try:
                result = subprocess.run(
                    ["curl", "-s", f"http://127.0.0.1:{port}/status"],
                    capture_output=True,
                    timeout=3
                )
                if result.returncode == 0 and result.stdout:
                    status = json.loads(result.stdout)
                    print(f" ✅ Ready ({i+1}s)")
                    return status
            except Exception:
                pass
            
            if (i + 1) % 10 == 0:
                print(f" {i+1}s...", end="", flush=True)
        
        raise RuntimeError(f"Server on port {port} not ready after {max_wait}s")
    
    def run_inference(self, port: int, prompt: str, model_name: str) -> tuple:
        """Run inference and return (output, usage, elapsed_time)"""
        payload = {
            "model": model_name,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": MAX_TOKENS,
            "temperature": TEMPERATURE,
            "top_p": TOP_P
        }
        
        start = time.time()
        result = subprocess.run(
            [
                "curl", "-s", "-X", "POST",
                f"http://127.0.0.1:{port}/v1/chat/completions",
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
        
        response = json.loads(result.stdout)
        
        if "error" in response:
            raise RuntimeError(f"API error: {response['error']}")
        
        content = response["choices"][0]["message"]["content"]
        usage = response.get("usage", {})
        
        return content, usage, elapsed


def analyze_quality(text: str) -> tuple:
    """
    Analyze output quality.
    Returns (passed, issues_list)
    """
    issues = []
    
    # Check minimum length
    if len(text.strip()) < 50:
        issues.append(f"Output too short: {len(text)} chars")
    
    # Check for immediate word repetition (3+ times)
    words = text.lower().split()
    if len(words) > 10:
        for i in range(len(words) - 2):
            if words[i] == words[i+1] == words[i+2]:
                issues.append(f"Word repetition loop: '{words[i]}' x3")
                break
    
    # Check for phrase repetition
    sentences = [s.strip() for s in text.split('.') if s.strip()]
    if len(sentences) >= 2:
        for i in range(len(sentences) - 1):
            if sentences[i] and sentences[i] == sentences[i+1]:
                issues.append(f"Sentence repetition: '{sentences[i][:50]}...'")
                break
    
    # Check alphanumeric ratio (detect gibberish)
    alphanum_ratio = sum(c.isalnum() or c.isspace() for c in text) / max(len(text), 1)
    if alphanum_ratio < 0.80:
        issues.append(f"Low alphanumeric ratio: {alphanum_ratio:.1%} (possible gibberish)")
    
    passed = len(issues) == 0
    return passed, issues


def test_baseline(runner: ServerRunner) -> dict:
    """Test baseline model"""
    print(f"\n{'='*70}")
    print("Testing: Baseline PQ2_0")
    print(f"{'='*70}")
    print(f"  Model: {BASELINE_MODEL}")
    
    # Start server
    cmd = [
        str(BINARY),
        "serve",
        "--model", BASELINE_MODEL,
        "--port", str(PORT_BASELINE)
    ]
    print(f"  Command: {' '.join(cmd)}")
    
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    runner.procs.append(proc)
    
    try:
        # Wait for ready
        status = runner.wait_for_server(PORT_BASELINE)
        model_name = status.get('model', BASELINE_MODEL)
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = runner.run_inference(PORT_BASELINE, TEST_PROMPT, model_name)
        
        # Analyze quality
        passed, issues = analyze_quality(content)
        
        # Calculate metrics
        completion_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tokens_per_sec = completion_tokens / elapsed if elapsed > 0 else 0
        
        # Print results
        print(f"\n  📊 Results:")
        print(f"     Tokens: {completion_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tokens_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        
        if issues:
            print(f"\n  ⚠️  Quality Issues:")
            for issue in issues:
                print(f"     - {issue}")
        
        print(f"\n  📄 Output preview (first 100 tokens):")
        preview_words = content.split()[:100]
        preview = ' '.join(preview_words)
        for i in range(0, len(preview), 80):
            print(f"     {preview[i:i+80]}")
        
        return {
            "name": "baseline",
            "model": BASELINE_MODEL,
            "output": content,
            "output_length": len(content),
            "word_count": len(content.split()),
            "usage": usage,
            "elapsed_seconds": elapsed,
            "tokens_per_second": tokens_per_sec,
            "quality_passed": passed,
            "quality_issues": issues
        }
    
    finally:
        # Stop server
        print(f"  Stopping server...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        runner.procs.remove(proc)
        time.sleep(2)


def test_hybrid(runner: ServerRunner) -> dict:
    """Test hybrid model using Python server"""
    print(f"\n{'='*70}")
    print("Testing: Hybrid Model (PQ1/PQ2)")
    print(f"{'='*70}")
    print(f"  Model dir: {HYBRID_MODEL_DIR}")
    print(f"  GGUF file: {HYBRID_GGUF.name}")
    
    if not HYBRID_MODEL_DIR.exists():
        return {
            "name": "hybrid",
            "model": str(HYBRID_MODEL_DIR),
            "output": None,
            "quality_passed": None,
            "error": "Directory not found"
        }
    
    if not HYBRID_GGUF.exists():
        return {
            "name": "hybrid",
            "model": str(HYBRID_GGUF),
            "output": None,
            "quality_passed": None,
            "error": "GGUF file not found"
        }
    
    size_gb = HYBRID_GGUF.stat().st_size / 1e9
    print(f"  Size: {size_gb:.2f} GB")
    
    # Build command using Python server module
    # Use system Python3 which should have transformers
    cmd = [
        "python3", "-u", "-P", "-m", "server.server",
        str(HYBRID_MODEL_DIR),
        "--tokenizer", str(HYBRID_MODEL_DIR),  # tokenizer in root dir
        "--model", "prism-ml/hybrid-test",  # Must be in owner/repo format
        "--binary", str(WORKDIR / "build/splash"),
        "--port", str(PORT_HYBRID),
        "--kv-format=int4",
        "--vision-text-only",
        "--kv-swap-enable=on",
        "--kv-swap-hot-tier=200000",
        "--enable-weight-prefetch=on"
    ]
    print(f"  Command: {' '.join(cmd)}")
    
    # Set environment
    env = os.environ.copy()
    env["PYTHONPATH"] = str(WORKDIR)
    env["PYTHONUNBUFFERED"] = "1"
    env["TRANSFORMERS_VERBOSITY"] = "error"
    
    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=str(WORKDIR)
    )
    runner.procs.append(proc)
    
    try:
        # Wait for ready (hybrid model may take longer to load)
        status = runner.wait_for_server(PORT_HYBRID, max_wait=120)
        model_name = status.get('model', 'prism-ml/hybrid-test')
        
        # Run inference
        print(f"  Running inference...")
        content, usage, elapsed = runner.run_inference(PORT_HYBRID, TEST_PROMPT, model_name)
        
        # Analyze quality
        passed, issues = analyze_quality(content)
        
        # Calculate metrics
        completion_tokens = usage.get("completion_tokens", 0)
        total_tokens = usage.get("total_tokens", 0)
        tokens_per_sec = completion_tokens / elapsed if elapsed > 0 else 0
        
        # Print results
        print(f"\n  📊 Results:")
        print(f"     Tokens: {completion_tokens} completion / {total_tokens} total")
        print(f"     Speed: {tokens_per_sec:.1f} tok/s")
        print(f"     Time: {elapsed:.1f}s")
        print(f"     Quality: {'✅ PASS' if passed else '❌ FAIL'}")
        
        if issues:
            print(f"\n  ⚠️  Quality Issues:")
            for issue in issues:
                print(f"     - {issue}")
        
        print(f"\n  📄 Output preview (first 100 tokens):")
        preview_words = content.split()[:100]
        preview = ' '.join(preview_words)
        for i in range(0, len(preview), 80):
            print(f"     {preview[i:i+80]}")
        
        return {
            "name": "hybrid",
            "model": str(HYBRID_GGUF),
            "output": content,
            "output_length": len(content),
            "word_count": len(content.split()),
            "usage": usage,
            "elapsed_seconds": elapsed,
            "tokens_per_second": tokens_per_sec,
            "quality_passed": passed,
            "quality_issues": issues
        }
    
    except Exception as e:
        print(f"\n  ❌ Error: {e}")
        import traceback
        traceback.print_exc()
        return {
            "name": "hybrid",
            "model": str(HYBRID_GGUF),
            "output": None,
            "quality_passed": None,
            "error": str(e)
        }
    
    finally:
        # Stop server
        print(f"  Stopping server...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        runner.procs.remove(proc)
        time.sleep(2)


def compare_results(results: list) -> bool:
    """Compare baseline and hybrid results"""
    print(f"\n{'='*70}")
    print("COMPARISON REPORT")
    print(f"{'='*70}")
    
    baseline = results[0]
    hybrid = results[1]
    
    if hybrid.get("output") is None:
        print(f"\n⚠️  Hybrid model was not tested - manual verification needed")
        print(f"✅ Baseline model passed quality checks: {baseline['quality_passed']}")
        return False
    
    # Quality comparison
    print(f"\n1. Quality Assessment:")
    print(f"   Baseline (PQ2_0):  {'✅ PASS' if baseline['quality_passed'] else '❌ FAIL'}")
    print(f"   Hybrid:            {'✅ PASS' if hybrid['quality_passed'] else '❌ FAIL'}")
    
    # Performance comparison (if available)
    if 'tokens_per_second' in baseline and 'tokens_per_second' in hybrid:
        print(f"\n2. Performance:")
        print(f"   Baseline:  {baseline['tokens_per_second']:6.1f} tok/s")
        print(f"   Hybrid:    {hybrid['tokens_per_second']:6.1f} tok/s", end="")
        if hybrid['tokens_per_second'] > baseline['tokens_per_second']:
            delta = (hybrid['tokens_per_second'] / baseline['tokens_per_second'] - 1) * 100
            print(f"  ({delta:+.1f}%)")
        else:
            print()
    
    # Output samples
    if baseline.get("output") and hybrid.get("output"):
        print(f"\n3. Output Samples (first 100 tokens):")
        print(f"\n   Baseline:")
        baseline_words = baseline["output"].split()[:100]
        print(f"   {' '.join(baseline_words)}")
        print(f"\n   Hybrid:")
        hybrid_words = hybrid["output"].split()[:100]
        print(f"   {' '.join(hybrid_words)}")
    
    # Final verdict
    print(f"\n{'='*70}")
    if baseline['quality_passed'] and hybrid.get('quality_passed'):
        print("✅ OVERALL: PASS - Both models produce coherent output")
        if hybrid.get('tokens_per_second', 0) > baseline['tokens_per_second']:
            speedup = (hybrid['tokens_per_second'] / baseline['tokens_per_second'] - 1) * 100
            print(f"   Performance improvement: +{speedup:.1f}%")
        return True
    elif not hybrid.get('quality_passed'):
        print("❌ OVERALL: FAIL - Hybrid model shows quality degradation")
        return False
    else:
        print("⚠️  OVERALL: INCOMPLETE - Hybrid model not tested")
        return False


def main():
    print("="*70)
    print("Phase 2 Quality Regression Test")
    print("="*70)
    print(f"\nTest: Compare hybrid vs baseline model output quality")
    print(f"Prompt: '{TEST_PROMPT}'")
    print(f"Max tokens: {MAX_TOKENS}")
    print(f"Temperature: {TEMPERATURE}, Top-p: {TOP_P}\n")
    
    # Verify binary exists
    if not BINARY.exists():
        print(f"❌ Error: Binary not found: {BINARY}")
        return 1
    
    results = []
    
    with ServerRunner() as runner:
        try:
            # Test 1: Baseline
            results.append(test_baseline(runner))
            
            # Test 2: Hybrid (manual for now)
            results.append(test_hybrid(runner))
            
        except KeyboardInterrupt:
            print("\n\n⚠️  Test interrupted by user")
            return 130
        
        except Exception as e:
            print(f"\n\n❌ Test failed with error: {e}")
            import traceback
            traceback.print_exc()
            return 1
    
    # Save results
    output_file = WORKDIR / "phase2_quality_test_results.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\n\nDetailed results saved to: {output_file}")
    
    # Compare and generate report
    all_passed = compare_results(results)
    
    print(f"\n{'='*70}\n")
    
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
