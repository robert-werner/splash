// AsyncKvSwap stress test: verify no crash with repeated page touches
// Tests the fixed bugs:
// 1. RAM usage accounting (touchPage memory leak)
// 2. Race condition in workerLoop prefetchQueue_ access

#include "runtime/engine/AsyncKvSwap.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace splash::engine;

int main() {
  std::cout << "AsyncKvSwap stress test: 100+ consecutive page operations\n";
  
  AsyncKvSwap::Config config;
  config.enabled = true;
  config.swapDir = "/tmp/splash_test_swap";
  config.ramCapacityBytes = 1024 * 1024 * 100; // 100 MB
  config.swapThreshold = 0.90;
  
  AsyncKvSwap swap(config);
  swap.start();
  
  std::cout << "Starting 100 iterations...\n";
  
  for (uint32_t i = 0; i < 100; ++i) {
    // Touch same pages multiple times (tests RAM accounting bug)
    for (uint32_t pageId = 0; pageId < 10; ++pageId) {
      size_t pageSize = 1024 * 512; // 512 KB per page
      swap.touchPage(pageId, pageSize);
    }
    
    // Register cold pages
    for (uint32_t pageId = 0; pageId < 5; ++pageId) {
      swap.registerColdPage(pageId);
    }
    
    // Trigger swap-out
    swap.swapOutColdPages();
    
    // Sleep to allow background thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    if (i % 10 == 0) {
      auto metrics = swap.snapshotMetrics();
      std::cout << "Iteration " << i 
                << " · RAM usage: " << metrics.currentRamUsageBytes / 1024 << " KB"
                << " · Swap outs: " << metrics.swapOutCount
                << " · Swap ins: " << metrics.swapInCount << "\n";
    }
  }
  
  std::cout << "Test completed successfully!\n";
  
  auto finalMetrics = swap.snapshotMetrics();
  std::cout << "\nFinal metrics:\n"
            << "  RAM usage: " << finalMetrics.currentRamUsageBytes / 1024 << " KB\n"
            << "  Swap usage: " << finalMetrics.currentSwapUsageBytes / 1024 << " KB\n"
            << "  Swap outs: " << finalMetrics.swapOutCount << "\n"
            << "  Swap ins: " << finalMetrics.swapInCount << "\n"
            << "  Avg swap-out latency: " << swap.avgSwapOutLatencyMs() << " ms\n"
            << "  Avg swap-in latency: " << swap.avgSwapInLatencyMs() << " ms\n";
  
  swap.stop();
  
  return 0;
}
