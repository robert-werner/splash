#!/usr/bin/env python3
"""
Fast integration test for Quick Wins optimizations.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

def test_build():
    """Test 1: Binary built successfully."""
    binary = Path("build/splash")
    return binary.exists() and binary.stat().st_size > 1_000_000, "Binary built"

def test_help():
    """Test 2: Help shows parameters."""
    result = subprocess.run(["./build/splash", "serve", "--help"], 
                          capture_output=True, text=True, timeout=5)
    output = result.stdout + result.stderr
    params = ["--max-batch-size", "--dynamic-batching", "--continuous-attention-threshold"]
    missing = [p for p in params if p not in output]
    return len(missing) == 0, f"Help params: {len(params)-len(missing)}/{len(params)}"

def test_symbols():
    """Test 3: Key symbols compiled."""
    result = subprocess.run(["nm", "build/engine/libsplash.a"], 
                          capture_output=True, text=True, timeout=5)
    symbols = ["DynamicBatching", "MemoryTelemetry", "observeDecodeBatch"]
    found = sum(1 for s in symbols if s in result.stdout)
    return found >= 2, f"Symbols: {found}/3"

def test_params():
    """Test 4: Parameters accepted."""
    with tempfile.TemporaryDirectory() as tmpdir:
        result = subprocess.run(
            ["./build/splash", "serve-native", "--max-batch-size", "8", 
             "--dynamic-batching", tmpdir],
            capture_output=True, text=True, timeout=5
        )
        output = result.stdout + result.stderr
        return "unexpected argument" not in output.lower(), "Params accepted"

def run_tests():
    tests = [
        ("Build", test_build),
        ("Help", test_help),
        ("Symbols", test_symbols),
        ("Params", test_params),
    ]
    
    print("=" * 60)
    print("QUICK WINS INTEGRATION TEST")
    print("=" * 60)
    
    passed = 0
    for name, func in tests:
        try:
            success, msg = func()
            status = "✅" if success else "❌"
            print(f"{status} {name}: {msg}")
            passed += success
        except Exception as e:
            print(f"❌ {name}: {e}")
    
    print("=" * 60)
    print(f"Result: {passed}/{len(tests)} passed")
    print("=" * 60)
    return passed == len(tests)

if __name__ == "__main__":
    sys.exit(0 if run_tests() else 1)
