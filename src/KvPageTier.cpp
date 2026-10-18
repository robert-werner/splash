#include "KvPageTier.hpp"
#include <algorithm>

namespace splash {

KvPageTier::KvPageTier() = default;
KvPageTier::~KvPageTier() = default;

void KvPageTier::setPageState(uint64_t pageId, PageState newState) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        pages_[pageId] = PageMetadata();
        it = pages_.find(pageId);
    }
    
    PageState oldState = it->second.state;
    if (oldState == newState) return;
    
    // Update counters
    switch (oldState) {
        case PageState::HOT_RESIDENT:
            hotCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        case PageState::COLD_RESIDENT:
            coldCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        case PageState::SWAPPED_OUT:
            swappedCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        case PageState::FAULTING:
            break;
    }
    
    switch (newState) {
        case PageState::HOT_RESIDENT:
            hotCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PageState::COLD_RESIDENT:
            coldCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PageState::SWAPPED_OUT:
            swappedCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PageState::FAULTING:
            break;
    }
    
    it->second.state = newState;
}

PageState KvPageTier::getPageState(uint64_t pageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    return (it != pages_.end()) ? it->second.state : PageState::HOT_RESIDENT;
}

bool KvPageTier::isSwapped(uint64_t pageId) const {
    return getPageState(pageId) == PageState::SWAPPED_OUT;
}

bool KvPageTier::isResident(uint64_t pageId) const {
    PageState state = getPageState(pageId);
    return state == PageState::HOT_RESIDENT || state == PageState::COLD_RESIDENT;
}

void KvPageTier::recordAccess(uint64_t pageId, uint64_t currentTick) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        pages_[pageId] = PageMetadata();
        it = pages_.find(pageId);
        hotCount_.fetch_add(1, std::memory_order_relaxed);
    }
    it->second.lastAccessTick = currentTick;
}

uint64_t KvPageTier::getLastAccess(uint64_t pageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    return (it != pages_.end()) ? it->second.lastAccessTick : 0;
}

void KvPageTier::setSwapMetadata(uint64_t pageId, uint64_t offset, 
                                  uint32_t compressedSize, uint32_t originalSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        pages_[pageId] = PageMetadata();
        it = pages_.find(pageId);
    }
    it->second.swapOffset = offset;
    it->second.compressedSize = compressedSize;
    it->second.originalSize = originalSize;
}

bool KvPageTier::getSwapMetadata(uint64_t pageId, uint64_t& offset, 
                                  uint32_t& compressedSize, uint32_t& originalSize) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it == pages_.end() || it->second.state != PageState::SWAPPED_OUT) {
        return false;
    }
    offset = it->second.swapOffset;
    compressedSize = it->second.compressedSize;
    originalSize = it->second.originalSize;
    return true;
}

void KvPageTier::acquireRef(uint64_t pageId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        pages_[pageId] = PageMetadata();
        it = pages_.find(pageId);
        hotCount_.fetch_add(1, std::memory_order_relaxed);
    }
    it->second.refCount.fetch_add(1, std::memory_order_acquire);
}

void KvPageTier::releaseRef(uint64_t pageId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    if (it != pages_.end()) {
        it->second.refCount.fetch_sub(1, std::memory_order_release);
    }
}

bool KvPageTier::canEvict(uint64_t pageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(pageId);
    return (it != pages_.end()) && (it->second.refCount.load(std::memory_order_acquire) == 0);
}

std::vector<uint64_t> KvPageTier::getLRUCandidates(size_t count, uint64_t minAge) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Collect evictable resident pages
    std::vector<std::pair<uint64_t, uint64_t>> candidates; // pageId, lastAccessTick
    candidates.reserve(pages_.size());
    
    for (const auto& [pageId, meta] : pages_) {
        if (meta.refCount.load(std::memory_order_acquire) == 0 &&
            (meta.state == PageState::HOT_RESIDENT || meta.state == PageState::COLD_RESIDENT) &&
            (minAge == 0 || meta.lastAccessTick <= minAge)) {
            candidates.emplace_back(pageId, meta.lastAccessTick);
        }
    }
    
    // Sort by LRU (oldest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    // Return top N page IDs
    std::vector<uint64_t> result;
    result.reserve(std::min(count, candidates.size()));
    for (size_t i = 0; i < std::min(count, candidates.size()); ++i) {
        result.push_back(candidates[i].first);
    }
    
    return result;
}

size_t KvPageTier::getHotPages() const {
    return hotCount_.load(std::memory_order_relaxed);
}

size_t KvPageTier::getColdPages() const {
    return coldCount_.load(std::memory_order_relaxed);
}

size_t KvPageTier::getSwappedPages() const {
    return swappedCount_.load(std::memory_order_relaxed);
}

void KvPageTier::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
    hotCount_.store(0, std::memory_order_relaxed);
    coldCount_.store(0, std::memory_order_relaxed);
    swappedCount_.store(0, std::memory_order_relaxed);
}

} // namespace splash
