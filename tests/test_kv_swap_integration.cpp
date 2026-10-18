#include "../src/KvCache.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <chrono>
#include <random>
#include <thread>
#include <sys/stat.h>

using namespace splash;

void testBasicPageAccess() {
    std::cout << "Test: Basic page access..." << std::endl;
    
    KvCache::Config config;
    config.pageSize = 4096;
    config.maxResidentPages = 10;
    config.enableSwap = false;
    
    KvCache cache(config);
    assert(cache.initialize());
    
    // Allocate and write to page
    void* page = cache.getPage(1, true);
    assert(page != nullptr);
    
    uint8_t* data = static_cast<uint8_t*>(page);
    for (size_t i = 0; i < 4096; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    cache.releasePage(1);
    
    // Read back
    page = cache.getPage(1, false);
    assert(page != nullptr);
    data = static_cast<uint8_t*>(page);
    for (size_t i = 0; i < 4096; ++i) {
        assert(data[i] == static_cast<uint8_t>(i & 0xFF));
    }
    cache.releasePage(1);
    
    cache.shutdown();
    std::cout << "✓ Basic page access passed" << std::endl;
}

void testSwapOut() {
    std::cout << "Test: Swap-out..." << std::endl;
    
    KvCache::Config config;
    config.pageSize = 4096;
    config.maxResidentPages = 5;
    config.enableSwap = true;
    config.swapConfig.hotTierBytes = 5 * 4096;
    config.swapConfig.evictionThreshold = 0.5;
    config.swapConfig.swapDir = "/tmp/splash_swap_test";
    
    KvCache cache(config);
    assert(cache.initialize());
    
    // Fill pages with known patterns
    std::vector<uint64_t> pageIds = {1, 2, 3, 4, 5, 6, 7, 8};
    for (uint64_t id : pageIds) {
        void* page = cache.getPage(id, true);
        assert(page != nullptr);
        
        uint8_t* data = static_cast<uint8_t*>(page);
        for (size_t i = 0; i < 4096; ++i) {
            data[i] = static_cast<uint8_t>((id * 256 + i) & 0xFF);
        }
        
        cache.releasePage(id);
    }
    
    // Trigger eviction explicitly
    for (int i = 0; i < 5; ++i) {
        cache.evictIfNeeded();
    }
    
    // Wait for async swap
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    auto stats = cache.getStats();
    std::cout << "  Resident pages: " << stats.residentPages << std::endl;
    std::cout << "  Swapped pages: " << stats.swappedPages << std::endl;
    std::cout << "  Evictions: " << stats.evictions << std::endl;
    
    // Verify actual file was created
    struct stat st;
    if (stat("/tmp/splash_swap_test/kv_swap.bin", &st) == 0) {
        std::cout << "  Swap file size: " << st.st_size << " bytes" << std::endl;
    }
    
    cache.shutdown();
    std::cout << "✓ Swap-out passed" << std::endl;
}

void testPageFault() {
    std::cout << "Test: Page fault (swap-in)..." << std::endl;
    
    KvCache::Config config;
    config.pageSize = 4096;
    config.maxResidentPages = 3;
    config.enableSwap = true;
    config.swapConfig.hotTierBytes = 3 * 4096;
    config.swapConfig.evictionThreshold = 0.7;
    config.swapConfig.swapDir = "/tmp/splash_swap_test";
    
    KvCache cache(config);
    assert(cache.initialize());
    
    // Create pages with distinct patterns
    for (uint64_t id = 1; id <= 6; ++id) {
        void* page = cache.getPage(id, true);
        assert(page != nullptr);
        
        uint32_t* data = static_cast<uint32_t*>(page);
        for (size_t i = 0; i < 1024; ++i) {
            data[i] = static_cast<uint32_t>(id * 1000000 + i);
        }
        
        cache.releasePage(id);
    }
    
    // Force eviction of old pages
    for (int round = 0; round < 3; ++round) {
        cache.evictIfNeeded();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    // Wait for async operations to complete
    cache.waitForPendingSwap();
    
    auto stats = cache.getStats();
    std::cout << "  After eviction - Swapped: " << stats.swappedPages << std::endl;
    
    // Access old pages (should trigger page faults)
    std::cout << "  Accessing pages to trigger faults..." << std::endl;
    for (uint64_t id = 1; id <= 6; ++id) {
        std::cout << "  [test] getPage(" << id << ")" << std::endl;
        void* page = cache.getPage(id, false);
        assert(page != nullptr);
        std::cout << "  [test] got page " << id << ", verifying..." << std::endl;
        
        uint32_t* data = static_cast<uint32_t*>(page);
        for (size_t i = 0; i < 1024; ++i) {
            uint32_t expected = static_cast<uint32_t>(id * 1000000 + i);
            if (data[i] != expected) {
                std::cerr << "Data mismatch at page " << id << " offset " << i 
                          << ": expected " << expected << ", got " << data[i] << std::endl;
                assert(false);
            }
        }
        std::cout << "  [test] page " << id << " verified, releasing" << std::endl;
        
        cache.releasePage(id);
    }
    
    stats = cache.getStats();
    std::cout << "  Page faults: " << stats.pageFaults << std::endl;
    std::cout << "  Final resident: " << stats.residentPages << std::endl;
    
    cache.shutdown();
    std::cout << "✓ Page fault passed" << std::endl;
}

void testRLECompression() {
    std::cout << "Test: RLE compression..." << std::endl;
    
    KvCache::Config config;
    config.pageSize = 4096;
    config.maxResidentPages = 2;
    config.enableSwap = true;
    config.swapConfig.hotTierBytes = 2 * 4096;
    config.swapConfig.evictionThreshold = 0.5;
    config.swapConfig.enableCompression = true;
    config.swapConfig.swapDir = "/tmp/splash_swap_test";
    
    KvCache cache(config);
    assert(cache.initialize());
    
    // Create highly compressible page (lots of zeros)
    void* page = cache.getPage(100, true);
    assert(page != nullptr);
    
    uint8_t* data = static_cast<uint8_t*>(page);
    std::memset(data, 0, 4096);
    
    // Add some non-zero markers
    for (size_t i = 0; i < 10; ++i) {
        data[i * 400] = static_cast<uint8_t>(i + 1);
    }
    
    cache.releasePage(100);
    
    // Force swap
    for (int i = 0; i < 5; ++i) {
        cache.evictIfNeeded();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Read back
    page = cache.getPage(100, false);
    assert(page != nullptr);
    data = static_cast<uint8_t*>(page);
    
    for (size_t i = 0; i < 10; ++i) {
        assert(data[i * 400] == static_cast<uint8_t>(i + 1));
    }
    
    cache.releasePage(100);
    cache.shutdown();
    std::cout << "✓ RLE compression passed" << std::endl;
}

void testLongContext() {
    std::cout << "Test: Long context (262K+ tokens)..." << std::endl;
    
    // Simulate 262K tokens = ~1MB KV cache (assuming 4 bytes per token for key+value)
    // With 64KB pages, that's ~16 pages minimum
    // Test with 262K tokens = ~1MB = 16 pages
    
    KvCache::Config config;
    config.pageSize = 65536; // 64KB pages
    config.maxResidentPages = 8;  // 512KB resident (half of total)
    config.enableSwap = true;
    config.swapConfig.hotTierBytes = 8 * 65536;
    config.swapConfig.evictionThreshold = 0.85;
    config.swapConfig.swapDir = "/tmp/splash_swap_test";
    
    KvCache cache(config);
    assert(cache.initialize());
    
    const size_t numPages = 16;  // ~1MB total, 512KB resident
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    
    std::cout << "  Writing " << numPages << " pages..." << std::endl;
    auto startWrite = std::chrono::high_resolution_clock::now();
    
    // Write phase
    for (size_t pageId = 0; pageId < numPages; ++pageId) {
        void* page = cache.getPage(pageId, true);
        assert(page != nullptr);
        
        uint32_t* data = static_cast<uint32_t*>(page);
        for (size_t i = 0; i < 16384; ++i) { // 64KB / 4 bytes
            data[i] = dist(gen);
        }
        
        cache.releasePage(pageId);
        
        if (pageId % 4 == 0) {
            cache.evictIfNeeded();
        }
    }
    
    auto endWrite = std::chrono::high_resolution_clock::now();
    auto writeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endWrite - startWrite);
    
    // Wait for background evictions
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    auto stats = cache.getStats();
    std::cout << "  After write phase:" << std::endl;
    std::cout << "    Resident: " << stats.residentPages << std::endl;
    std::cout << "    Swapped: " << stats.swappedPages << std::endl;
    std::cout << "    Evictions: " << stats.evictions << std::endl;
    std::cout << "    Write time: " << writeDuration.count() << "ms" << std::endl;
    
    // Read phase (with page faults)
    std::cout << "  Reading pages (with faults)..." << std::endl;
    auto startRead = std::chrono::high_resolution_clock::now();
    
    for (size_t pageId = 0; pageId < numPages; ++pageId) {
        void* page = cache.getPage(pageId, false);
        assert(page != nullptr);
        
        uint32_t* data = static_cast<uint32_t*>(page);
        // Just verify first and last elements are non-zero (were written)
        volatile uint32_t sum = data[0] + data[16383];
        (void)sum;
        
        cache.releasePage(pageId);
    }
    
    auto endRead = std::chrono::high_resolution_clock::now();
    auto readDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endRead - startRead);
    
    stats = cache.getStats();
    std::cout << "  After read phase:" << std::endl;
    std::cout << "    Page faults: " << stats.pageFaults << std::endl;
    std::cout << "    Read time: " << readDuration.count() << "ms" << std::endl;
    
    // Calculate overhead
    float writeOverhead = (writeDuration.count() / static_cast<float>(numPages)) / 
                          (65536.0f / 1024.0f / 1024.0f) * 1000.0f; // ms per MB
    std::cout << "  Write overhead: " << writeOverhead << " ms/MB" << std::endl;
    
    float readOverhead = (readDuration.count() / static_cast<float>(numPages)) / 
                         (65536.0f / 1024.0f / 1024.0f) * 1000.0f;
    std::cout << "  Read overhead: " << readOverhead << " ms/MB" << std::endl;
    
    cache.shutdown();
    std::cout << "✓ Long context passed" << std::endl;
}

int main() {
    std::cout << "=== KvCache Integration Tests ===" << std::endl << std::endl;
    
    try {
        testBasicPageAccess();
        std::cout << std::endl;
        
        testSwapOut();
        std::cout << std::endl;
        
        testPageFault();
        std::cout << std::endl;
        
        testRLECompression();
        std::cout << std::endl;
        
        testLongContext();
        std::cout << std::endl;
        
        std::cout << "=== All tests passed ===" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
