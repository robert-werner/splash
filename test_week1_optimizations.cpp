#include "ops/KvCompression.hpp"
#include "engine/PrefillChunking.hpp"
#include "engine/DynamicBatching.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace splash;

void testKvCompression() {
  std::cout << "Testing KV Compression..." << std::endl;

  // Test 1: Compress data with runs
  std::vector<std::byte> source(1024);
  std::memset(source.data(), 0, 256);
  std::memset(source.data() + 256, 1, 256);
  std::memset(source.data() + 512, 2, 256);
  std::memset(source.data() + 768, 3, 256);

  kv::KvCompression::CompressionStats stats;
  auto compressed = kv::KvCompression::compress(source, stats);

  assert(stats.compressed && "Compression should succeed");
  assert(stats.runCount == 4 && "Should find 4 runs");
  assert(stats.compressedBytes < stats.originalBytes && "Compressed should be smaller");
  std::cout << "  Original: " << stats.originalBytes << " bytes" << std::endl;
  std::cout << "  Compressed: " << stats.compressedBytes << " bytes" << std::endl;
  std::cout << "  Ratio: " << stats.compressionRatio << std::endl;

  // Test 2: Decompress
  std::vector<std::byte> decompressed(source.size());
  bool success = kv::KvCompression::decompress(compressed, decompressed);
  assert(success && "Decompression should succeed");
  assert(std::memcmp(source.data(), decompressed.data(), source.size()) == 0);
  std::cout << "  ✅ KV Compression tests passed" << std::endl;
}

void testPrefillChunking() {
  std::cout << "\nTesting Prefill Chunking..." << std::endl;

  engine::PrefillChunking chunker(8192);

  auto chunks1 = chunker.planChunks(4096);
  assert(chunks1.empty() && "Small prefill should not chunk");
  assert(chunker.chunkCount(4096) == 1);

  auto chunks3 = chunker.planChunks(20000);
  assert(!chunks3.empty() && "Large prefill should chunk");
  std::cout << "  Large prefill (20000 tokens): " << chunker.chunkCount(20000) << " chunks" << std::endl;

  std::cout << "  ✅ Prefill Chunking tests passed" << std::endl;
}

void testDynamicBatching() {
  std::cout << "\nTesting Dynamic Batching..." << std::endl;

  engine::DynamicBatching batcher(4, 0.98, 150.0);
  assert(batcher.currentMaxBatchSize() == 4);

  for (int i = 0; i < 10; ++i) {
    batcher.observeDecodeBatch(4, 76.0, 4, 0.98);
  }
  std::cout << "  Batch size after normal performance: " << batcher.currentMaxBatchSize() << std::endl;

  std::cout << "  ✅ Dynamic Batching tests passed" << std::endl;
}

int main() {
  std::cout << "=== Week 1 Optimizations Tests ===" << std::endl;

  try {
    testKvCompression();
    testPrefillChunking();
    testDynamicBatching();
    std::cout << "\n=== All tests passed ✅ ===" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "\n✗ Test failed: " << e.what() << std::endl;
    return 1;
  }
}
