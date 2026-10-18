#include "AsyncKvSwap.hpp"
#include "KvPageTier.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>

namespace splash {

AsyncKvSwap::AsyncKvSwap(const Config& config, KvPageTier* pageTier)
    : config_(config), pageTier_(pageTier) {}

AsyncKvSwap::~AsyncKvSwap() {
    shutdown();
}

bool AsyncKvSwap::initialize() {
    // Create swap directory
    mkdir(config_.swapDir.c_str(), 0755);
    
    // Open swap file
    std::string swapPath = config_.swapDir + "/kv_swap.bin";
    swapFd_ = open(swapPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (swapFd_ < 0) {
        std::cerr << "Failed to open swap file: " << swapPath << std::endl;
        return false;
    }
    
    // Start I/O threads
    running_.store(true, std::memory_order_release);
    for (size_t i = 0; i < config_.ioThreads; ++i) {
        ioThreads_.emplace_back(&AsyncKvSwap::ioWorker, this);
    }
    
    return true;
}

void AsyncKvSwap::shutdown() {
    running_.store(false, std::memory_order_release);
    queueCV_.notify_all();
    
    for (auto& thread : ioThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    ioThreads_.clear();
    
    if (swapFd_ >= 0) {
        close(swapFd_);
        swapFd_ = -1;
    }
}

void AsyncKvSwap::swapOutAsync(uint64_t pageId, const void* data, size_t dataSize,
                                std::function<void(bool)> callback) {
    SwapRequest req;
    req.type = SwapRequest::SWAP_OUT;
    req.pageId = pageId;
    req.data = const_cast<void*>(data);
    req.dataSize = dataSize;
    req.callback = std::move(callback);
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        requestQueue_.push(std::move(req));
    }
    queueCV_.notify_one();
}

void AsyncKvSwap::swapInAsync(uint64_t pageId, void* destBuffer, size_t bufferSize,
                               std::function<void(bool)> callback) {
    SwapRequest req;
    req.type = SwapRequest::SWAP_IN;
    req.pageId = pageId;
    req.data = destBuffer;
    req.dataSize = bufferSize;
    req.callback = std::move(callback);
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        requestQueue_.push(std::move(req));
    }
    queueCV_.notify_one();
}

bool AsyncKvSwap::swapInSync(uint64_t pageId, void* destBuffer, size_t bufferSize) {
    return readPageFromDisk(pageId, destBuffer, bufferSize);
}

bool AsyncKvSwap::shouldEvict() const {
    float usage = static_cast<float>(currentHotUsage_.load(std::memory_order_relaxed)) / 
                  static_cast<float>(config_.hotTierBytes);
    return usage >= config_.evictionThreshold;
}

size_t AsyncKvSwap::getCurrentUsage() const {
    return currentHotUsage_.load(std::memory_order_relaxed);
}

AsyncKvSwap::Stats AsyncKvSwap::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void AsyncKvSwap::ioWorker() {
    while (running_.load(std::memory_order_acquire)) {
        SwapRequest req;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] {
                return !requestQueue_.empty() || !running_.load(std::memory_order_acquire);
            });
            
            if (!running_.load(std::memory_order_acquire)) break;
            if (requestQueue_.empty()) continue;
            
            req = std::move(requestQueue_.front());
            requestQueue_.pop();
        }
        
        bool success = false;
        
        if (req.type == SwapRequest::SWAP_OUT) {
            uint64_t offset;
            uint32_t compressedSize;
            success = writePageToDisk(req.pageId, req.data, req.dataSize, offset, compressedSize);
            
            if (success && pageTier_) {
                pageTier_->setSwapMetadata(req.pageId, offset, compressedSize, 
                                           static_cast<uint32_t>(req.dataSize));
                pageTier_->setPageState(req.pageId, PageState::SWAPPED_OUT);
            }
        } else {
            success = readPageFromDisk(req.pageId, req.data, req.dataSize);
            
            if (success && pageTier_) {
                pageTier_->setPageState(req.pageId, PageState::HOT_RESIDENT);
            }
        }
        
        if (req.callback) {
            req.callback(success);
        }
    }
}

bool AsyncKvSwap::writePageToDisk(uint64_t pageId, const void* data, size_t dataSize,
                                   uint64_t& outOffset, uint32_t& outCompressedSize) {
    if (swapFd_ < 0) return false;
    
    std::vector<uint8_t> compressed;
    const void* writeData = data;
    size_t writeSize = dataSize;
    
    if (config_.enableCompression) {
        compressed = compressRLE(data, dataSize);
        if (!compressed.empty() && compressed.size() < dataSize) {
            writeData = compressed.data();
            writeSize = compressed.size();
            
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.compressionSavedBytes += (dataSize - compressed.size());
        }
    }
    
    outOffset = swapFileOffset_.fetch_add(writeSize, std::memory_order_relaxed);
    outCompressedSize = static_cast<uint32_t>(writeSize);
    
    ssize_t written = pwrite(swapFd_, writeData, writeSize, static_cast<off_t>(outOffset));
    
    if (written == static_cast<ssize_t>(writeSize)) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.swapOutCount++;
        stats_.bytesWritten += writeSize;
        return true;
    }
    
    return false;
}

bool AsyncKvSwap::readPageFromDisk(uint64_t pageId, void* buffer, size_t bufferSize) {
    if (swapFd_ < 0 || !pageTier_) return false;
    
    uint64_t offset;
    uint32_t compressedSize, originalSize;
    if (!pageTier_->getSwapMetadata(pageId, offset, compressedSize, originalSize)) {
        return false;
    }
    
    if (originalSize > bufferSize) return false;
    
    std::vector<uint8_t> compressed(compressedSize);
    ssize_t readBytes = pread(swapFd_, compressed.data(), compressedSize, static_cast<off_t>(offset));
    
    if (readBytes != static_cast<ssize_t>(compressedSize)) {
        return false;
    }
    
    bool success = false;
    if (config_.enableCompression && compressedSize < originalSize) {
        success = decompressRLE(compressed.data(), compressedSize, buffer, originalSize);
    } else {
        std::memcpy(buffer, compressed.data(), compressedSize);
        success = true;
    }
    
    if (success) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.swapInCount++;
        stats_.bytesRead += compressedSize;
    }
    
    return success;
}

std::vector<uint8_t> AsyncKvSwap::compressRLE(const void* data, size_t size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> compressed;
    compressed.reserve(size / 2); // Optimistic estimate
    
    size_t i = 0;
    while (i < size) {
        uint8_t value = src[i];
        size_t runLength = 1;
        
        while (i + runLength < size && src[i + runLength] == value && runLength < 255) {
            runLength++;
        }
        
        if (runLength >= 3) {
            // RLE encoding: [0xFF] [count] [value]
            compressed.push_back(0xFF);
            compressed.push_back(static_cast<uint8_t>(runLength));
            compressed.push_back(value);
            i += runLength;
        } else {
            // Literal
            compressed.push_back(value);
            i++;
        }
    }
    
    return compressed;
}

bool AsyncKvSwap::decompressRLE(const void* compressed, size_t compressedSize,
                                 void* output, size_t outputSize) {
    const uint8_t* src = static_cast<const uint8_t*>(compressed);
    uint8_t* dst = static_cast<uint8_t*>(output);
    
    size_t srcIdx = 0;
    size_t dstIdx = 0;
    
    while (srcIdx < compressedSize && dstIdx < outputSize) {
        if (src[srcIdx] == 0xFF && srcIdx + 2 < compressedSize) {
            // RLE run
            size_t count = src[srcIdx + 1];
            uint8_t value = src[srcIdx + 2];
            
            if (dstIdx + count > outputSize) return false;
            
            std::memset(dst + dstIdx, value, count);
            dstIdx += count;
            srcIdx += 3;
        } else {
            // Literal
            dst[dstIdx++] = src[srcIdx++];
        }
    }
    
    return dstIdx == outputSize;
}

} // namespace splash
