#include "KvCache.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace splash {

KvCache::KvCache(const Config& config)
    : config_(config)
    , pageTier_(std::make_unique<KvPageTier>())
    , asyncSwap_(nullptr)
{
    if (config_.enableSwap) {
        asyncSwap_ = std::make_unique<AsyncKvSwap>(config_.swapConfig, pageTier_.get());
    }
}

KvCache::~KvCache() {
    shutdown();
}

bool KvCache::initialize() {
    if (config_.enableSwap && asyncSwap_) {
        if (!asyncSwap_->initialize()) {
            std::cerr << "Failed to initialize AsyncKvSwap" << std::endl;
            return false;
        }
    }
    return true;
}

void KvCache::shutdown() {
    if (asyncSwap_) {
        asyncSwap_->shutdown();
    }
    
    std::lock_guard<std::mutex> lock(cacheMutex_);
    pages_.clear();
    pageTier_->clear();
}

void* KvCache::getPage(uint64_t pageId, bool willWrite) {
    uint64_t tick = currentTick_.fetch_add(1, std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    // Check if page is resident
    auto it = pages_.find(pageId);
    if (it != pages_.end()) {
        pageTier_->recordAccess(pageId, tick);
        if (willWrite) {
            it->second->dirty = true;
        }
        return it->second->data;
    }
    
    // Page fault: check if swapped
    if (pageTier_->isSwapped(pageId)) {
        pageFaultCount_.fetch_add(1, std::memory_order_relaxed);
        
        if (!handlePageFault(pageId)) {
            std::cerr << "Page fault failed for page " << pageId << std::endl;
            return nullptr;
        }
        
        // Page should now be resident
        it = pages_.find(pageId);
        if (it != pages_.end()) {
            pageTier_->recordAccess(pageId, tick);
            if (willWrite) {
                it->second->dirty = true;
            }
            return it->second->data;
        }
    }
    
    // Page doesn't exist, allocate new
    auto page = std::make_unique<KvPage>();
    page->data = allocatePage(config_.pageSize);
    page->size = config_.pageSize;
    page->pageId = pageId;
    page->dirty = false;
    
    if (!page->data) {
        return nullptr;
    }
    
    void* dataPtr = page->data;
    pages_[pageId] = std::move(page);
    
    pageTier_->setPageState(pageId, PageState::HOT_RESIDENT);
    pageTier_->recordAccess(pageId, tick);
    
    return dataPtr;
}

void KvCache::releasePage(uint64_t pageId) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    pageTier_->releaseRef(pageId);
}

void KvCache::registerColdPage(uint64_t pageId) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        return;
    }
    
    // Move to cold tier (would compress in real implementation)
    pageTier_->setPageState(pageId, PageState::COLD_RESIDENT);
}

void KvCache::evictIfNeeded() {
    if (!config_.enableSwap || !asyncSwap_) {
        return;
    }
    
    // Check if we're over the resident page limit
    size_t residentCount;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        residentCount = pages_.size();
    }
    
    if (residentCount <= config_.maxResidentPages * 0.9) {
        return;
    }
    
    // Select LRU candidates - no minAge filter for aggressive eviction
    uint64_t currentTick = currentTick_.load(std::memory_order_relaxed);
    
    size_t evictCount = residentCount - (config_.maxResidentPages * 0.8);
    std::vector<uint64_t> candidates = pageTier_->getLRUCandidates(evictCount, 0);
    
    std::cout << "Evicting " << candidates.size() << " pages (resident: " 
              << residentCount << "/" << config_.maxResidentPages << ")" << std::endl;
    
    for (uint64_t pageId : candidates) {
        evictPage(pageId);
    }
}

std::vector<uint64_t> KvCache::selectEvictionCandidates(size_t count) {
    uint64_t currentTick = currentTick_.load(std::memory_order_relaxed);
    uint64_t minAge = currentTick > 1000 ? currentTick - 1000 : 0;
    
    return pageTier_->getLRUCandidates(count, minAge);
}

KvCache::Stats KvCache::getStats() const {
    Stats stats;
    stats.residentPages = pageTier_->getHotPages() + pageTier_->getColdPages();
    stats.swappedPages = pageTier_->getSwappedPages();
    stats.pageFaults = pageFaultCount_.load(std::memory_order_relaxed);
    stats.evictions = evictionCount_.load(std::memory_order_relaxed);
    stats.currentTick = currentTick_.load(std::memory_order_relaxed);
    return stats;
}

bool KvCache::handlePageFault(uint64_t pageId) {
    if (!asyncSwap_) {
        return false;
    }
    
    // Allocate page for swap-in
    auto page = std::make_unique<KvPage>();
    page->data = allocatePage(config_.pageSize);
    page->size = config_.pageSize;
    page->pageId = pageId;
    page->dirty = false;
    
    if (!page->data) {
        return false;
    }
    
    // Mark as faulting
    pageTier_->setPageState(pageId, PageState::FAULTING);
    
    // Synchronous swap-in (page fault must complete before returning)
    bool success = asyncSwap_->swapInSync(pageId, page->data, config_.pageSize);
    
    if (success) {
        pages_[pageId] = std::move(page);
        pageTier_->setPageState(pageId, PageState::HOT_RESIDENT);
        return true;
    } else {
        pageTier_->setPageState(pageId, PageState::SWAPPED_OUT);
        return false;
    }
}

bool KvCache::evictPage(uint64_t pageId) {
    if (!asyncSwap_) {
        return false;
    }
    
    std::unique_ptr<KvPage> pageToEvict;
    
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        
        auto it = pages_.find(pageId);
        if (it == pages_.end()) {
            return false;
        }
        
        if (!pageTier_->canEvict(pageId)) {
            return false;
        }
        
        // Transfer ownership for async operation
        pageToEvict = std::move(it->second);
        pages_.erase(it);
    }
    
    // Now do async swap-out with owned data
    void* data = pageToEvict->data;
    size_t size = pageToEvict->size;
    
    // Transfer ownership to lambda
    asyncSwap_->swapOutAsync(pageId, data, size, 
        [this, pageId, pagePtr = pageToEvict.release()](bool success) {
            if (success) {
                evictionCount_.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Swap failed, restore page
                std::lock_guard<std::mutex> lock(cacheMutex_);
                pages_[pageId] = std::unique_ptr<KvPage>(pagePtr);
                pageTier_->setPageState(pageId, PageState::HOT_RESIDENT);
                return;
            }
            delete pagePtr;
        });
    
    return true;
}

void* KvCache::allocatePage(size_t size) {
    void* ptr = malloc(size);
    if (ptr) {
        std::memset(ptr, 0, size);
    }
    return ptr;
}

} // namespace splash
