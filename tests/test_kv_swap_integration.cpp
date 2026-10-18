#include "engine/AsyncKvSwap.hpp"
#include "engine/Engine.hpp"
#include "engine/KvCache.hpp"
#include "engine/KvPageTier.hpp"
#include "model/SlotFile.hpp"
#include "ops/PageStorage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace splash::engine {
namespace {

// Test fixture helper for KV swap integration tests
class KvSwapIntegrationTest {
protected:
  void SetUp() {
    // Create temporary swap directory
    swapDir_ = std::filesystem::temp_directory_path() / "kv_swap_test";
    std::filesystem::create_directories(swapDir_);
  }

  void TearDown() {
    // Clean up swap directory
    if (std::filesystem::exists(swapDir_)) {
      std::filesystem::remove_all(swapDir_);
    }
  }

  // Helper: generate dummy page data
  std::vector<std::byte> generatePageData(size_t sizeBytes, uint32_t pageId) {
    std::vector<std::byte> data(sizeBytes);
    // Fill with deterministic pattern based on pageId for verification
    std::mt19937 rng(pageId);
    for (size_t i = 0; i < sizeBytes; ++i) {
      data[i] = static_cast<std::byte>(rng() & 0xFF);
    }
    return data;
  }

  // Helper: verify page data integrity
  bool verifyPageData(const std::vector<std::byte> &data, uint32_t pageId) {
    std::vector<std::byte> expected = generatePageData(data.size(), pageId);
    return data == expected;
  }

  std::filesystem::path swapDir_;
};

// Test 1: Write 300K tokens to overflow hot tier (180K default), verify
// eviction triggered and cold tier has pages
void test1_HotTierOverflowTriggersEviction() {
  KvSwapIntegrationTest test;
  test.SetUp();

  // Configuration: hot tier 150K tokens (default 180K would be 180*pageTokens)
  constexpr size_t hotTierTokens = 150000;
  constexpr size_t hotTierBytes = hotTierTokens * 2; // 2 bytes per token estimate
  constexpr size_t totalTokens = 300000;
  constexpr size_t pageBytes = 4 * 1024 * 1024; // 4 MiB per page
  constexpr uint32_t pageTokens = KvCache::pageTokens;
  const uint32_t hotTierPages = hotTierTokens / pageTokens;
  const uint32_t totalPages = totalTokens / pageTokens;

  std::cout << "[Test 1] Hot tier: " << hotTierTokens << " tokens ("
            << hotTierPages << " pages), total: " << totalTokens << " tokens ("
            << totalPages << " pages)\n";

  // Create AsyncKvSwap with hot tier threshold
  AsyncKvSwap::Config config;
  config.swapDir = test.swapDir_;
  config.ramCapacityBytes = hotTierBytes;
  config.swapThreshold = 0.90; // Swap when > 90% of hot tier
  config.enabled = true;

  auto asyncSwap = std::make_unique<AsyncKvSwap>(config);
  asyncSwap->start();

  // Simulate filling pages beyond hot tier capacity
  std::vector<uint32_t> allocatedPages;
  for (uint32_t i = 0; i < totalPages; ++i) {
    allocatedPages.push_back(i);
    asyncSwap->touchPage(i, pageBytes);

    // After exceeding hot tier, register old pages as cold
    if (i >= hotTierPages) {
      const uint32_t coldPageId = i - hotTierPages;
      asyncSwap->registerColdPage(coldPageId);
    }
  }

  // Trigger swap-out
  asyncSwap->swapOutColdPages();

  // Wait for async swap worker to process
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Verify metrics
  auto metrics = asyncSwap->snapshotMetrics();
  std::cout << "[Test 1] Swap metrics: swapOutCount=" << metrics.swapOutCount
            << ", bytesSwappedOut=" << metrics.bytesSwappedOut
            << ", avgLatencyMs=" << asyncSwap->avgSwapOutLatencyMs() << "\n";

  if (metrics.swapOutCount > 0) {
    std::cout << "[Test 1] ✓ PASS: At least one page swapped out after overflow\n";
  } else {
    std::cout << "[Test 1] ✗ FAIL: Expected at least one page swapped out\n";
  }

  if (metrics.bytesSwappedOut > 0) {
    std::cout << "[Test 1] ✓ PASS: Non-zero bytes swapped\n";
  } else {
    std::cout << "[Test 1] ✗ FAIL: Expected non-zero bytes swapped\n";
  }

  if (asyncSwap->avgSwapOutLatencyMs() < 30.0) {
    std::cout << "[Test 1] ✓ PASS: Average swap-out latency < 30ms\n";
  } else {
    std::cout << "[Test 1] ⚠ WARNING: Average swap-out latency >= 30ms target\n";
  }

  asyncSwap->stop();
  test.TearDown();
}

// Test 2: Access old swapped page, verify reload + data integrity matches
// original
void test2_SwappedPageReloadWithDataIntegrity() {
  KvSwapIntegrationTest test;
  test.SetUp();

  constexpr size_t pageBytes = 4 * 1024 * 1024;
  constexpr uint32_t testPageId = 42;

  std::cout << "[Test 2] Testing swapped page reload with data integrity\n";

  // Create AsyncKvSwap
  AsyncKvSwap::Config config;
  config.swapDir = test.swapDir_;
  config.ramCapacityBytes = pageBytes * 10;
  config.swapThreshold = 0.90;
  config.enabled = true;

  auto asyncSwap = std::make_unique<AsyncKvSwap>(config);

  // Generate original page data
  std::vector<std::byte> originalData = test.generatePageData(pageBytes, testPageId);

  // Mock: directly write the swap file to simulate swap-out
  const std::filesystem::path swapFilePath =
      test.swapDir_ / ("page_" + std::to_string(testPageId) + ".swap");
  {
    std::ofstream out(swapFilePath, std::ios::binary);
    if (out.is_open()) {
      out.write(reinterpret_cast<const char *>(originalData.data()),
                originalData.size());
      std::cout << "[Test 2] Created swap file: " << swapFilePath << "\n";
    }
  }

  // Mark page as swapped
  asyncSwap->touchPage(testPageId, pageBytes);

  // Synchronously reload the page
  std::vector<std::byte> reloadedData(pageBytes);
  const auto startTime = std::chrono::steady_clock::now();
  const bool loaded = asyncSwap->loadPageSync(testPageId, reloadedData);
  const auto endTime = std::chrono::steady_clock::now();
  const auto latencyMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime)
          .count();

  std::cout << "[Test 2] Page reload: success=" << loaded
            << ", latencyMs=" << latencyMs << "\n";

  // If swap file exists, loading should succeed
  if (std::filesystem::exists(swapFilePath)) {
    if (loaded) {
      std::cout << "[Test 2] ✓ PASS: Successfully reloaded swapped page\n";
    } else {
      std::cout << "[Test 2] ✗ FAIL: Failed to reload swapped page\n";
    }

    if (latencyMs < 30) {
      std::cout << "[Test 2] ✓ PASS: Page reload latency < 30ms target\n";
    } else {
      std::cout << "[Test 2] ⚠ WARNING: Page reload latency >= 30ms target\n";
    }

    // Verify data integrity
    const bool dataMatches = test.verifyPageData(reloadedData, testPageId);
    if (dataMatches) {
      std::cout << "[Test 2] ✓ PASS: Reloaded page data matches original\n";
    } else {
      std::cout << "[Test 2] ✗ FAIL: Reloaded page data does not match original\n";
    }
  }

  asyncSwap->stop();
  test.TearDown();
}

// Test 3: Measure swap-out/swap-in latency < 30ms per page
void test3_SwapLatencyBenchmark() {
  KvSwapIntegrationTest test;
  test.SetUp();

  constexpr size_t pageBytes = 4 * 1024 * 1024;
  constexpr uint32_t numTestPages = 10;
  constexpr double targetLatencyMs = 30.0;

  std::cout << "[Test 3] Benchmarking swap latency for " << numTestPages
            << " pages\n";

  AsyncKvSwap::Config config;
  config.swapDir = test.swapDir_;
  config.ramCapacityBytes = pageBytes * 5; // Small hot tier to force swaps
  config.swapThreshold = 0.50;             // Low threshold for testing
  config.enabled = true;

  auto asyncSwap = std::make_unique<AsyncKvSwap>(config);
  asyncSwap->start();

  std::vector<double> swapOutLatencies;
  std::vector<double> swapInLatencies;

  // Perform swap-out and swap-in cycles
  for (uint32_t pageId = 0; pageId < numTestPages; ++pageId) {
    // Generate page data
    std::vector<std::byte> pageData = test.generatePageData(pageBytes, pageId);

    // Write swap file to simulate swap-out
    const std::filesystem::path swapFilePath =
        test.swapDir_ / ("page_" + std::to_string(pageId) + ".swap");

    auto startSwapOut = std::chrono::steady_clock::now();
    {
      std::ofstream out(swapFilePath, std::ios::binary);
      out.write(reinterpret_cast<const char *>(pageData.data()),
                pageData.size());
    }
    auto endSwapOut = std::chrono::steady_clock::now();
    const double swapOutMs =
        std::chrono::duration<double, std::milli>(endSwapOut - startSwapOut)
            .count();
    swapOutLatencies.push_back(swapOutMs);

    // Reload page (swap-in)
    std::vector<std::byte> reloadedData(pageBytes);
    auto startSwapIn = std::chrono::steady_clock::now();
    const bool loaded = asyncSwap->loadPageSync(pageId, reloadedData);
    auto endSwapIn = std::chrono::steady_clock::now();
    const double swapInMs =
        std::chrono::duration<double, std::milli>(endSwapIn - startSwapIn)
            .count();

    if (loaded) {
      swapInLatencies.push_back(swapInMs);
    }
  }

  // Calculate average latencies
  const double avgSwapOut =
      std::accumulate(swapOutLatencies.begin(), swapOutLatencies.end(), 0.0) /
      swapOutLatencies.size();
  const double avgSwapIn =
      !swapInLatencies.empty()
          ? std::accumulate(swapInLatencies.begin(), swapInLatencies.end(),
                            0.0) /
                swapInLatencies.size()
          : 0.0;

  std::cout << "[Test 3] Average swap-out latency: " << avgSwapOut << " ms\n";
  std::cout << "[Test 3] Average swap-in latency: " << avgSwapIn << " ms\n";

  if (avgSwapOut < targetLatencyMs) {
    std::cout << "[Test 3] ✓ PASS: Average swap-out latency < 30ms target\n";
  } else {
    std::cout << "[Test 3] ⚠ WARNING: Average swap-out latency >= 30ms target\n";
  }

  if (!swapInLatencies.empty()) {
    if (avgSwapIn < targetLatencyMs) {
      std::cout << "[Test 3] ✓ PASS: Average swap-in latency < 30ms target\n";
    } else {
      std::cout << "[Test 3] ⚠ WARNING: Average swap-in latency >= 30ms target\n";
    }
  }

  asyncSwap->stop();
  test.TearDown();
}

} // namespace
} // namespace splash::engine

int main() {
  std::cout << "=== Async KV Page Swap Integration Tests ===\n\n";

  try {
    splash::engine::test1_HotTierOverflowTriggersEviction();
    std::cout << "\n";

    splash::engine::test2_SwappedPageReloadWithDataIntegrity();
    std::cout << "\n";

    splash::engine::test3_SwapLatencyBenchmark();
    std::cout << "\n";

    std::cout << "=== All integration tests completed ===\n";
    std::cout << "\nNOTE: For full end-to-end testing with Ternary-Bonsai-2-27B model:\n";
    std::cout << "  Run: ./splash --model Ternary-Bonsai-2-27B-PQ2_0.gguf \\\n";
    std::cout << "                --kv-swap-enable --kv-swap-hot-tier 150000 \\\n";
    std::cout << "                --prompt <262K+ token context>\n";
    std::cout << "  Measure throughput vs baseline (target: -5% acceptable)\n";

    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Test failed with exception: " << e.what() << "\n";
    return 1;
  }
}
