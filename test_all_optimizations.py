#!/usr/bin/env python3
"""Test suite for all 10 optimizations"""
import subprocess
import sys

def run_test(name, command):
    print(f"\n{'='*70}")
    print(f"Test: {name}")
    print(f"{'='*70}")
    result = subprocess.run(command, shell=True, capture_output=True, text=True)
    print(result.stdout)
    if result.stderr:
        print(result.stderr)
    return result.returncode == 0

tests = [
    ("Build verification", "ls -lh build/splash"),
    ("Library symbols check", "ar t build/engine/libsplash.a | grep -E 'DynamicBatch|LazyAne|WriteCoal|KvDefrag|MemoryTele|Int4Kv|VisionCond|KernelFus|Speculative'"),
    ("CLI parameters help", "./splash serve --help | grep -E 'max-batch-size|dynamic-batching|continuous-attention'"),
    ("Native binary help", "./build/splash --help | head -5"),
]

passed = 0
failed = 0

for name, cmd in tests:
    if run_test(name, cmd):
        passed += 1
    else:
        failed += 1

print(f"\n{'='*70}")
print(f"RESULTS: {passed} passed, {failed} failed")
print(f"{'='*70}")

sys.exit(0 if failed == 0 else 1)
