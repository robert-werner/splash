#pragma once

#include "KvPageTier.hpp"
#include "AsyncKvSwap.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace splash {

struct KvPage {
    void* data;
    size_t size;
    uint64_t pageId;
    bool dirty;
    
    KvPage() : data(nullptr), size(0), pageId(0), dirty(false) {}
    ~KvPage() {
        if (data) {
            free(data);
            data = nullptr;
        }
    }
    
    KvPage(const KvPage&) = delete;
    KvPage& operator=(const KvPage&) = delete;
    
    KvPage(KvPage&& other) noexcept
        : data(other.data)
        , size(other.size)
        , pageId(other.pageId)
        , dirty(other.dirty) {
        other.data = nullptr;
        other.size = 0;
    }
    
    KvPage& operator=(KvPage&& other) noexcept {
        if (this != &other) {
            if (data) free(data);
            data = other.data;
            size = other.size;
            pageId = other.pageId;
            dirty = other.dirty;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
};

class KvCache {
public:
    struct Config {
        size_t pageSize;              // Size of each page in bytes
        size_t maxResidentPages;      // Max pages in hot tier
        bool enableSwap;              // Enable SSD swapping
        AsyncKvSwap::Config swapConfig;
        
        Config()
            : pageSize(64 * 1024)     // 64KB pages
            , maxResidentPages(2800)  // ~180MB at 64KB/page
            , enableSwap(false)
        {}
    };
    
    explicit KvCache(const Config& config);
    ~KvCache();
    
    bool initialize();
    void shutdown();
    
    // Page access (with automatic page fault handling)
    void* getPage(uint64_t pageId, bool willWrite = false);
    void releasePage(uint64_t pageId);
    
    // Cold page registration (called by eviction logic)
    void registerColdPage(uint64_t pageId);
    
    // Memory pressure management
    void evictIfNeeded();
    std::vector<uint64_t> selectEvictionCandidates(size_t count);
    
    // Statistics
    struct Stats {
        size_t residentPages;
        size_t swappedPages;
        uint64_t pageFaults;
        uint64_t evictions;
        uint64_t currentTick;
    };
    Stats getStats() const;
    
    // Wait for pending async operations
    void waitForPendingSwap();
    
private:
    bool handlePageFault(uint64_t pageId);
    bool evictPage(uint64_t pageId);
    void* allocatePage(size_t size);
    
    Config config_;
    std::unique_ptr<KvPageTier> pageTier_;
    std::unique_ptr<AsyncKvSwap> asyncSwap_;
    
    mutable std::mutex cacheMutex_;
    std::unordered_map<uint64_t, std::unique_ptr<KvPage>> pages_;
    
    std::atomic<uint64_t> currentTick_{0};
    std::atomic<uint64_t> pageFaultCount_{0};
    std::atomic<uint64_t> evictionCount_{0};
};

} // namespace splash
