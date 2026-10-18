#!/bin/bash
set -e

echo "Compiling KV swap integration test (simplified)..."

# Compile the test file
xcrun -sdk macosx clang++ -std=c++20 -O0 -g -Wall -Wno-error \
    -I../runtime \
    -c test_kv_swap_integration.cpp -o test_kv_swap_integration_new.o 2>&1 | head -50

echo "✓ Compilation completed"
ls -lh test_kv_swap_integration_new.o
