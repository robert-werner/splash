#!/bin/bash
set -e

echo "Compiling KV swap integration test..."

xcrun -sdk macosx clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -I../runtime \
    -I../build/engine \
    -c ../runtime/engine/AsyncKvSwap.cpp -o AsyncKvSwap.o

xcrun -sdk macosx clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -I../runtime \
    -I../build/engine \
    -c ../runtime/engine/KvPageTier.cpp -o KvPageTier.o

xcrun -sdk macosx clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -I../runtime \
    -I../build/engine \
    -c test_kv_swap_integration.cpp -o test_kv_swap_integration.o

xcrun -sdk macosx clang++ -std=c++20 -O2 \
    test_kv_swap_integration.o AsyncKvSwap.o KvPageTier.o \
    ../build/engine/libsplash.a \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    -o test_kv_swap_integration

echo "✓ Compilation successful"
echo "Run with: ./test_kv_swap_integration"
