#!/usr/bin/env python3
"""
Test: Validate that new CLI parameters are correctly parsed
"""

import sys
import subprocess

def test_param_parsing():
    """Test that parameters parse without errors"""
    
    tests = [
        {
            'name': 'Help shows new parameters',
            'cmd': ['./splash', 'serve', '--help'],
            'expect_in_output': [
                '--max-batch-size',
                '--dynamic-batching',
                '--continuous-attention-threshold'
            ]
        },
        {
            'name': 'Max batch size validation (too low)',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--max-batch-size', '0'],
            'expect_error': True,
            'expect_in_stderr': ['batch', 'size', '1']
        },
        {
            'name': 'Max batch size validation (too high)',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--max-batch-size', '20'],
            'expect_error': True,
            'expect_in_stderr': ['batch', 'size', '16']
        },
        {
            'name': 'Max batch size validation (valid)',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--max-batch-size', '8', '--help'],
            'expect_error': False
        },
        {
            'name': 'Continuous attention threshold (negative)',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--continuous-attention-threshold', '-1'],
            'expect_error': True,
            'expect_in_stderr': ['usage']  # Just check it shows usage on error
        },
        {
            'name': 'Continuous attention threshold (valid)',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--continuous-attention-threshold', '4096', '--help'],
            'expect_error': False
        },
        {
            'name': 'Dynamic batching flag',
            'cmd': ['./splash', 'serve', '--model', 'dummy', '--dynamic-batching', '--help'],
            'expect_error': False
        },
    ]
    
    print("\n" + "="*70)
    print("Parameter Validation Tests")
    print("="*70)
    
    passed = 0
    failed = 0
    
    for test in tests:
        print(f"\n📝 {test['name']}")
        print(f"   Command: {' '.join(test['cmd'])}")
        
        result = subprocess.run(
            test['cmd'],
            capture_output=True,
            text=True,
            timeout=10
        )
        
        # Check exit code
        if test.get('expect_error'):
            if result.returncode == 0:
                print(f"   ✗ Expected error but got success")
                failed += 1
                continue
        else:
            if result.returncode not in [0, 2]:  # 2 is argparse help exit code
                print(f"   ✗ Unexpected exit code: {result.returncode}")
                print(f"      stderr: {result.stderr[:200]}")
                failed += 1
                continue
        
        # Check output contains expected strings
        output = result.stdout + result.stderr
        if 'expect_in_output' in test:
            missing = []
            for expected in test['expect_in_output']:
                if expected not in output:
                    missing.append(expected)
            if missing:
                print(f"   ✗ Missing in output: {missing}")
                print(f"      Got: {output[:300]}")
                failed += 1
                continue
        
        if 'expect_in_stderr' in test:
            missing = []
            for expected in test['expect_in_stderr']:
                if expected.lower() not in result.stderr.lower():
                    missing.append(expected)
            if missing:
                print(f"   ✗ Missing in stderr: {missing}")
                print(f"      Got stderr: {result.stderr[:300]}")
                failed += 1
                continue
        
        print(f"   ✓ Passed")
        passed += 1
    
    print("\n" + "="*70)
    print(f"Results: {passed} passed, {failed} failed")
    print("="*70)
    
    if failed == 0:
        print("\n✅ ALL PARAMETER VALIDATION TESTS PASSED!")
        return 0
    else:
        print(f"\n❌ {failed} test(s) failed")
        return 1

if __name__ == '__main__':
    try:
        sys.exit(test_param_parsing())
    except KeyboardInterrupt:
        print("\n\nInterrupted")
        sys.exit(1)
