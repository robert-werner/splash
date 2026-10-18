#!/bin/bash
echo "=== Phase 3 Integration Verification ==="
echo ""
echo "1. Page fault handler in KvCache::find():"
grep -A 5 "Phase 3: Page fault handler - synchronously reload" runtime/engine/KvCache.cpp | head -6
echo ""
echo "2. Page fault handler in KvCache::insert():"
grep -A 5 "Phase 3: Page fault handler before lookup" runtime/engine/KvCache.cpp | head -6
echo ""
echo "3. AsyncKvSwap integration in KvCache.hpp:"
grep -A 2 "AsyncKvSwap integration" runtime/engine/KvCache.hpp
echo ""
echo "4. Integration test file exists:"
ls -lh tests/test_kv_swap_integration.cpp
echo ""
echo "5. Build status (-Werror):"
make -j8 2>&1 | tail -1
echo ""
echo "=== Verification complete ==="
