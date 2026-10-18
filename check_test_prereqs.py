#!/usr/bin/env python3
"""
Check if prerequisites for quality regression test are met
"""

import sys
from pathlib import Path

WORKDIR = Path(__file__).parent.resolve()
BINARY = WORKDIR / "splash"
BUILD_BINARY = WORKDIR / "build" / "splash"
HYBRID_MODEL = WORKDIR / "install/models/Ternary-Bonsai-2-27B-hybrid-ptq1pq2.gguf"

print("Checking prerequisites for quality regression test:\n")

all_good = True

# Check binary
print("1. Splash binary:")
if BINARY.exists():
    print(f"   ✅ {BINARY} exists")
elif BUILD_BINARY.exists():
    print(f"   ✅ {BUILD_BINARY} exists")
    print(f"   ⚠️  Note: Tests expect ./splash wrapper, not build/splash")
else:
    print(f"   ❌ Binary not found")
    print(f"      Expected: {BINARY}")
    print(f"      Status: Waiting for subagent #1 to complete build")
    all_good = False

# Check hybrid model
print("\n2. Hybrid GGUF model:")
if HYBRID_MODEL.exists():
    size_gb = HYBRID_MODEL.stat().st_size / (1024**3)
    print(f"   ✅ {HYBRID_MODEL.name}")
    print(f"      Size: {size_gb:.2f} GB")
else:
    print(f"   ❌ Model not found: {HYBRID_MODEL}")
    all_good = False

# Check ports
print("\n3. Required ports (8090-8092):")
import socket
for port in [8090, 8091, 8092]:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(1)
        result = sock.connect_ex(('127.0.0.1', port))
        sock.close()
        if result == 0:
            print(f"   ⚠️  Port {port} is in use")
        else:
            print(f"   ✅ Port {port} is available")
    except Exception as e:
        print(f"   ⚠️  Port {port}: {e}")

print("\n" + "="*70)
if all_good:
    print("✅ All prerequisites met - ready to run quality_regression_test.py")
    sys.exit(0)
else:
    print("❌ Prerequisites not met - waiting for:")
    if not BINARY.exists() and not BUILD_BINARY.exists():
        print("   - Subagent #1: Build completion")
    print("\nRun this script again once builds complete.")
    sys.exit(1)
