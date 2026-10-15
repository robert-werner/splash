#!/usr/bin/env python3
"""Test Phase 3 runtime integration"""

import subprocess
import sys

def test_cli_flags():
    """Test that all Phase 3 CLI flags are recognized"""
    result = subprocess.run(
        ["build/splash", "serve-native", "--help"],
        capture_output=True,
        text=True
    )
    
    help_text = result.stdout + result.stderr
    
    flags = [
        "--int4-kv-cache",
        "--vision-text-only",
        "--kernel-fusion",
        "--speculative-decoding"
    ]
    
    missing = []
    for flag in flags:
        if flag not in help_text:
            missing.append(flag)
    
    if missing:
        print(f"❌ Missing CLI flags: {missing}")
        return False
    
    print(f"✅ All Phase 3 CLI flags present")
    return True

def test_build_artifacts():
    """Test that all Phase 3 components are in the binary"""
    result = subprocess.run(
        ["ar", "t", "build/engine/libsplash.a"],
        capture_output=True,
        text=True
    )
    
    components = [
        "Int4KvCache.o",
        "VisionConditional.o",
        "KernelFusion.o",
        "SpeculativeDecodingV2.o"
    ]
    
    missing = []
    for comp in components:
        if comp not in result.stdout:
            missing.append(comp)
    
    if missing:
        print(f"❌ Missing components in libsplash.a: {missing}")
        return False
    
    print(f"✅ All Phase 3 components in binary")
    return True

def test_binary_size():
    """Check binary size is reasonable"""
    result = subprocess.run(
        ["ls", "-lh", "build/splash"],
        capture_output=True,
        text=True
    )
    
    if "2.5M" not in result.stdout and "2.4M" not in result.stdout and "2.6M" not in result.stdout:
        print(f"⚠️  Binary size unexpected: {result.stdout}")
        return False
    
    print(f"✅ Binary size reasonable: ~2.5M")
    return True

def main():
    tests = [
        ("CLI Flags", test_cli_flags),
        ("Build Artifacts", test_build_artifacts),
        ("Binary Size", test_binary_size)
    ]
    
    passed = 0
    failed = 0
    
    for name, test_fn in tests:
        print(f"\nTesting {name}...")
        try:
            if test_fn():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"❌ {name} raised exception: {e}")
            failed += 1
    
    print(f"\n{'='*60}")
    print(f"Phase 3 Integration Tests: {passed}/{len(tests)} passed")
    print(f"{'='*60}")
    
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
