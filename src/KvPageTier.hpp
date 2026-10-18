#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace splash {

enum class PageState : uint8_t {
    HOT_RESIDENT = 0,    // In hot tier (GPU/system RAM)
    COLD_RESIDENT = 1,   // In cold tier (compressed in RAM)
    SWAPPED_OUT = 2,     // On SSD, not in memory
    FAULTING = 3         // Currently being loaded from SSD
};

struct PageMetadata {
    PageState state;
    uint64_t lastAccessTick;
    uint64_t swapOffset;      // Offset in swap file
    uint32_t compressedSize;  // Size on disk (RLE compressed)
    uint32_t originalSize;    // Uncompressed size
    std::atomic<uint32_t> refCount; // Active references
    
    PageMetadata() 
        : state(PageState::HOT_RESIDENT)
        , lastAccessTick(0)
        , swapOffset(0)
        , compressedSize(0)
        , originalSize(0)
        , refCount(0)
    {}
    
    // Allow move but not copy (atomic refCount prevents implicit copy)
    PageMetadata(const PageMetadata& other)
        : state(other.state)
        , lastAccessTick(other.lastAccessTick)
        , swapOffset(other.swapOffset)
        , compressedSize(other.compressedSize)
        , originalSize(other.originalSize)
        , refCount(other.refCount.load(std::memory_order_relaxed))
    {}
    
    PageMetadata& operator=(const PageMetadata& other) {
        if (this != &other) {
            state = other.state;
            lastAccessTick = other.lastAccessTick;
            swapOffset = other.swapOffset;
            compressedSize = other.compressedSize;
            originalSize = other.originalSize;
            refCount.store(other.refCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }
};

class KvPageTier {
public:
    KvPageTier();
    ~KvPageTier();

    // State management
    void setPageState(uint64_t pageId, PageState newState);
    PageState getPageState(uint64_t pageId) const;
    bool isSwapped(uint64_t pageId) const;
    bool isResident(uint64_t pageId) const;
    
    // Access tracking for LRU
    void recordAccess(uint64_t pageId, uint64_t currentTick);
    uint64_t getLastAccess(uint64_t pageId) const;
    
    // Swap metadata
    void setSwapMetadata(uint64_t pageId, uint64_t offset, uint32_t compressedSize, uint32_t originalSize);
    bool getSwapMetadata(uint64_t pageId, uint64_t& offset, uint32_t& compressedSize, uint32_t& originalSize) const;
    
    // Reference counting for safe eviction
    void acquireRef(uint64_t pageId);
    void releaseRef(uint64_t pageId);
    bool canEvict(uint64_t pageId) const;
    
    // LRU candidate selection
    std::vector<uint64_t> getLRUCandidates(size_t count, uint64_t minAge) const;
    
    // Statistics
    size_t getHotPages() const;
    size_t getColdPages() const;
    size_t getSwappedPages() const;
    
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PageMetadata> pages_;
    
    // Fast state counters
    std::atomic<size_t> hotCount_{0};
    std::atomic<size_t> coldCount_{0};
    std::atomic<size_t> swappedCount_{0};
};

} // namespace splash
