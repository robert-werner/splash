#!/usr/bin/env python3
"""
Quality Regression Test: Hybrid GGUF Model with ANE Layers

Tests hybrid quantization quality by comparing:
1. Baseline model (pure PQ2_0, no hybrid quantization)
2. Hybrid model without ANE (to measure hybrid quantization impact)
3. Hybrid model with ANE layers 0,2,4,8,10,14 (full system test)

Generates 200 tokens and checks for:
- Coherent output (no gibberish)
- No repetition loops
- Reasonable output quality
- Performance metrics

Usage:
    python3 quality_regression_test.py
    
Requirements:
    - ./splash binary must be built
    - Models must be available
    - Port 8090-8092 must be free
"""

import subprocess
import json
import time
import sys
import os
import signal
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Configuration
WORKDIR = Path(__file__).parent.resolve()
BINARY = WORKDIR / "splash"
NATIVE_BINARY = WORKDIR / "build" / "splash"
BASELINE_MODEL = "prism-ml/Ternary-Bonsai-2-27B-gguf:PQ2_0"
HYBRID_MODEL_DIR = WORKDIR / "install/models/hybrid-test"

# Test parameters
TEST_PROMPT = "Explain the theory of relativity in simple terms."
MAX_TOKENS = 200
TEMPERATURE = 0.7
TOP_P = 0.9

# Ports for different tests
PORT_BASELINE = 8090
PORT_HYBRID_NO_ANE = 8091
PORT_HYBRID_WITH_ANE = 8092


class TestRunner:
    def __init__(self):
        self.procs: List[subprocess.Popen] = []
        
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
    
    def wait_for_server(self, port: int, max_wait: int = 60) -> Dict:
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
    
    def run_inference(self, port: int, prompt: str, model_name: str) -> Tuple[str, Dict, float]:
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
    
    def analyze_quality(self, text: str) -> Tuple[bool, List[str]]:
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
        
        # Check for excessive special characters
        special_chars = sum(1 for c in text if not c.isalnum() and not c.isspace() and c not in '.,!?;:\'"-()[]')
        if special_chars > len(text) * 0.15:
            issues.append(f"Too many special characters: {special_chars}/{len(text)}")
        
        passed = len(issues) == 0
        return passed, issues
    
    def test_model(
        self,
        name: str,
        model_spec: str,
        port: int,
        ane_layers: Optional[str] = None,
        use_native: bool = False
    ) -> Dict:
        """Test one model configuration"""
        print(f"\n{'='*70}")
        print(f"{name}")
        print(f"{'='*70}")
        print(f"  Model: {model_spec}")
        if ane_layers:
            print(f"  ANE layers: {ane_layers}")
        
        # Build command - use native binary for local directories
        if use_native:
            cmd = [
                str(NATIVE_BINARY),
                "serve-native",
                model_spec,
                "auto", "auto",  # context, cache
                "--port", str(port)
            ]
        else:
            cmd = [
                str(BINARY),
                "serve",
                "--model", model_spec,
                "--port", str(port)
            ]
        
        if ane_layers:
            cmd.extend(["--ane-layers", ane_layers])
        
        print(f"  Command: {' '.join(cmd)}")
        
        # Start server
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        self.procs.append(proc)
        
        try:
            # Wait for ready
            status = self.wait_for_server(port)
            print(f"  Context: {status.get('context_capacity', '?')} tokens")
            
            # Extract model name from status
            model_name = status.get('model', model_spec)
            
            # Run inference
            print(f"  Running inference...")
            content, usage, elapsed = self.run_inference(port, TEST_PROMPT, model_name)
            
            # Analyze quality
            passed, issues = self.analyze_quality(content)
            
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
            
            print(f"\n  📄 Output preview (first 300 chars):")
            preview = content[:300]
            for line in preview.split('\n'):
                print(f"     {line}")
            
            return {
                "name": name,
                "model": model_spec,
                "ane_layers": ane_layers,
                "output": content,
                "output_length": len(content),
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
            self.procs.remove(proc)
            time.sleep(2)  # Give port time to release


def compare_results(results: List[Dict]) -> bool:
    """
    Compare all test results and generate final report.
    Returns True if all tests passed.
    """
    print(f"\n{'='*70}")
    print("COMPARISON & FINAL REPORT")
    print(f"{'='*70}")
    
    baseline = results[0]
    hybrid_no_ane = results[1]
    hybrid_with_ane = results[2]
    
    all_passed = True
    
    # Quality comparison
    print(f"\n1. Quality Assessment:")
    print(f"   Baseline (PQ2_0):         {'✅ PASS' if baseline['quality_passed'] else '❌ FAIL'}")
    print(f"   Hybrid (no ANE):          {'✅ PASS' if hybrid_no_ane['quality_passed'] else '❌ FAIL'}")
    print(f"   Hybrid (with ANE):        {'✅ PASS' if hybrid_with_ane['quality_passed'] else '❌ FAIL'}")
    
    if not hybrid_with_ane['quality_passed']:
        all_passed = False
        print(f"\n   🚨 Hybrid with ANE failed quality checks:")
        for issue in hybrid_with_ane['quality_issues']:
            print(f"      - {issue}")
    
    # Performance comparison
    print(f"\n2. Performance:")
    print(f"   Baseline:      {baseline['tokens_per_second']:6.1f} tok/s")
    print(f"   Hybrid no ANE: {hybrid_no_ane['tokens_per_second']:6.1f} tok/s  ", end="")
    if hybrid_no_ane['tokens_per_second'] > baseline['tokens_per_second']:
        delta = (hybrid_no_ane['tokens_per_second'] / baseline['tokens_per_second'] - 1) * 100
        print(f"({delta:+.1f}%)")
    else:
        print()
    
    print(f"   Hybrid w/ ANE: {hybrid_with_ane['tokens_per_second']:6.1f} tok/s  ", end="")
    if hybrid_with_ane['tokens_per_second'] > baseline['tokens_per_second']:
        delta = (hybrid_with_ane['tokens_per_second'] / baseline['tokens_per_second'] - 1) * 100
        print(f"({delta:+.1f}%)")
    else:
        print()
    
    # Output length comparison
    print(f"\n3. Output Length:")
    print(f"   Baseline:      {baseline['output_length']} chars")
    print(f"   Hybrid no ANE: {hybrid_no_ane['output_length']} chars")
    print(f"   Hybrid w/ ANE: {hybrid_with_ane['output_length']} chars")
    
    # Sample outputs
    print(f"\n4. Output Samples (first 150 chars):")
    print(f"\n   Baseline:")
    print(f"   {baseline['output'][:150]}")
    print(f"\n   Hybrid no ANE:")
    print(f"   {hybrid_no_ane['output'][:150]}")
    print(f"\n   Hybrid with ANE:")
    print(f"   {hybrid_with_ane['output'][:150]}")
    
    # Final verdict
    print(f"\n{'='*70}")
    if all_passed:
        print("✅ OVERALL: PASS")
        print("   Hybrid GGUF with ANE layers maintains quality")
        if hybrid_with_ane['tokens_per_second'] > baseline['tokens_per_second']:
            speedup = (hybrid_with_ane['tokens_per_second'] / baseline['tokens_per_second'] - 1) * 100
            print(f"   Performance improvement: +{speedup:.1f}%")
    else:
        print("❌ OVERALL: FAIL")
        print("   Hybrid GGUF with ANE layers shows quality degradation")
    print(f"{'='*70}\n")
    
    return all_passed


def main():
    print("="*70)
    print("Quality Regression Test: Hybrid GGUF with ANE Layers")
    print("="*70)
    print(f"\nTest prompt: '{TEST_PROMPT}'")
    print(f"Max tokens: {MAX_TOKENS}")
    print(f"Temperature: {TEMPERATURE}, Top-p: {TOP_P}\n")
    
    # Verify binary exists
    if not BINARY.exists():
        print(f"❌ Error: Binary not found: {BINARY}")
        print("   Build the project first with: make")
        return 1
    
    if not NATIVE_BINARY.exists():
        print(f"❌ Error: Native binary not found: {NATIVE_BINARY}")
        print("   Build the native binary first")
        return 1
    
    # Verify hybrid model exists
    if not HYBRID_MODEL_DIR.exists():
        print(f"❌ Error: Hybrid model directory not found: {HYBRID_MODEL_DIR}")
        return 1
    
    results = []
    
    with TestRunner() as runner:
        try:
            # Test 1: Baseline PQ2_0 (reference)
            results.append(runner.test_model(
                "Baseline: Pure PQ2_0",
                BASELINE_MODEL,
                PORT_BASELINE
            ))
            
            # Test 2: Hybrid without ANE (measure hybrid quantization impact)
            results.append(runner.test_model(
                "Hybrid Quantization (no ANE)",
                str(HYBRID_MODEL_DIR),
                PORT_HYBRID_NO_ANE,
                use_native=True
            ))
            
            # Test 3: Hybrid with ANE layers (full system test)
            results.append(runner.test_model(
                "Hybrid Quantization + ANE Layers",
                str(HYBRID_MODEL_DIR),
                PORT_HYBRID_WITH_ANE,
                ane_layers="0,2,4,8,10,14",
                use_native=True
            ))
            
        except KeyboardInterrupt:
            print("\n\n⚠️  Test interrupted by user")
            return 130
        
        except Exception as e:
            print(f"\n\n❌ Test failed with error: {e}")
            import traceback
            traceback.print_exc()
            return 1
    
    # Save results
    output_file = WORKDIR / "quality_test_results.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nDetailed results saved to: {output_file}")
    
    # Compare and generate report
    all_passed = compare_results(results)
    
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
