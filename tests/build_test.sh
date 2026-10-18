#!/bin/bash
# Build script for KV swap integration tests

set -e

CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -O2 -g -Wall"
SRC_DIR="../src"
TEST_DIR="."

echo "Building KV swap components..."

# Compile source files
$CXX $CXXFLAGS -c "$SRC_DIR/KvPageTier.cpp" -o KvPageTier.o
$CXX $CXXFLAGS -c "$SRC_DIR/AsyncKvSwap.cpp" -o AsyncKvSwap.o
$CXX $CXXFLAGS -c "$SRC_DIR/KvCache.cpp" -o KvCache.o

# Compile test
$CXX $CXXFLAGS -c test_kv_swap_integration.cpp -o test_kv_swap_integration.o

# Link
$CXX $CXXFLAGS KvPageTier.o AsyncKvSwap.o KvCache.o test_kv_swap_integration.o -o test_kv_swap_integration -lpthread

echo "Build complete: ./test_kv_swap_integration"
