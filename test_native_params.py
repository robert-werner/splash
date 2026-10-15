#!/usr/bin/env python3
"""
Test: Native binary parameter parsing
"""

import sys
import subprocess
import tempfile

def test_native_params():
    """Test that native binary accepts new parameters"""
    
    tests = [
        {
            'name': 'Native help shows usage',
            'args': ['--help'],
            'expect_in_output': ['usage', 'model']
        },
        {
            'name': 'Max batch size parameter accepted',
            'args': ['serve-native', '/tmp', 'auto', 'auto', '--max-batch-size', '8'],
            'expect_any_exit': True  # Will fail on model loading but args should parse
        },
        {
            'name': 'Dynamic batching parameter accepted',
            'args': ['serve-native', '/tmp', 'auto', 'auto', '--dynamic-batching', 'on'],
            'expect_any_exit': True
        },
        {
            'name': 'Continuous attention threshold accepted',
            'args': ['serve-native', '/tmp', 'auto', 'auto', '--continuous-attention-threshold', '4096'],
            'expect_any_exit': True
        },
        {
            'name': 'All parameters together',
            'args': [
                'serve-native', '/tmp', 'auto', 'auto',
                '--max-batch-size', '8',
                '--dynamic-batching', 'on',
                '--continuous-attention-threshold', '4096'
            ],
            'expect_any_exit': True
        },
    ]
    
    print("\n" + "="*70)
    print("Native Binary Parameter Tests")
    print("="*70)
    
    passed = 0
    failed = 0
    
    binary = './build/splash'
    
    for test in tests:
        print(f"\n📝 {test['name']}")
        cmd = [binary] + test['args']
        print(f"   Command: {' '.join(cmd)}")
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=5
            )
            
            # Check success/failure
            if test.get('expect_success'):
                if result.returncode != 0:
                    print(f"   ✗ Expected success but got exit code {result.returncode}")
                    print(f"      stderr: {result.stderr[:300]}")
                    failed += 1
                    continue
            elif test.get('expect_any_exit'):
                # For expect_any_exit, just check that it doesn't report "unexpected argument"
                if 'unexpected argument --max-batch-size' in result.stderr or \
                   'unexpected argument --dynamic-batching' in result.stderr or \
                   'unexpected argument --continuous-attention-threshold' in result.stderr:
                    print(f"   ✗ Parameter not recognized by native binary")
                    print(f"      stderr: {result.stderr[:300]}")
                    failed += 1
                    continue
            
            # Check output
            output = result.stdout + result.stderr
            if 'expect_in_output' in test:
                missing = []
                for expected in test['expect_in_output']:
                    if expected.lower() not in output.lower():
                        missing.append(expected)
                if missing:
                    print(f"   ✗ Missing in output: {missing}")
                    print(f"      Got: {output[:300]}")
                    failed += 1
                    continue
            
            print(f"   ✓ Passed")
            passed += 1
            
        except subprocess.TimeoutExpired:
            print(f"   ✗ Timeout")
            failed += 1
        except Exception as e:
            print(f"   ✗ Error: {e}")
            failed += 1
    
    print("\n" + "="*70)
    print(f"Results: {passed} passed, {failed} failed")
    print("="*70)
    
    if failed == 0:
        print("\n✅ ALL NATIVE BINARY TESTS PASSED!")
        return 0
    else:
        print(f"\n❌ {failed} test(s) failed")
        return 1

if __name__ == '__main__':
    try:
        sys.exit(test_native_params())
    except KeyboardInterrupt:
        print("\n\nInterrupted")
        sys.exit(1)
