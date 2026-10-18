#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>

namespace splash {

// Forward declaration
class KvPageTier;

struct SwapRequest {
    enum Type { SWAP_OUT, SWAP_IN } type;
    uint64_t pageId;
    void* data;           // Source for SWAP_OUT, destination for SWAP_IN
    size_t dataSize;
    std::function<void(bool)> callback; // Called on completion
};

class AsyncKvSwap {
public:
    struct Config {
        std::string swapDir;          // Directory for swap files
        size_t hotTierBytes;          // Hot tier capacity
        float evictionThreshold;      // Trigger swap when usage > this ratio
        size_t coldTierBytes;         // Cold tier (compressed RAM) capacity
        bool enableCompression;       // RLE compression on cold tier
        size_t ioThreads;             // Number of async I/O threads
        
        Config()
            : swapDir("/tmp/splash_swap")
            , hotTierBytes(180000 * 1024)  // ~180MB default
            , evictionThreshold(0.90f)
            , coldTierBytes(360000 * 1024)
            , enableCompression(true)
            , ioThreads(2)
        {}
    };
    
    explicit AsyncKvSwap(const Config& config, KvPageTier* pageTier);
    ~AsyncKvSwap();
    
    // Lifecycle
    bool initialize();
    void shutdown();
    
    // Async swap operations
    void swapOutAsync(uint64_t pageId, const void* data, size_t dataSize, 
                      std::function<void(bool)> callback);
    void swapInAsync(uint64_t pageId, void* destBuffer, size_t bufferSize,
                     std::function<void(bool)> callback);
    
    // Synchronous fallback for page faults
    bool swapInSync(uint64_t pageId, void* destBuffer, size_t bufferSize);
    
    // Memory pressure queries
    bool shouldEvict() const;
    size_t getCurrentUsage() const;
    size_t getHotTierCapacity() const { return config_.hotTierBytes; }
    
    // Statistics
    struct Stats {
        uint64_t swapOutCount;
        uint64_t swapInCount;
        uint64_t bytesWritten;
        uint64_t bytesRead;
        uint64_t compressionSavedBytes;
    };
    Stats getStats() const;
    
private:
    void ioWorker();
    bool writePageToDisk(uint64_t pageId, const void* data, size_t dataSize, 
                         uint64_t& outOffset, uint32_t& outCompressedSize);
    bool readPageFromDisk(uint64_t pageId, void* buffer, size_t bufferSize);
    
    std::vector<uint8_t> compressRLE(const void* data, size_t size);
    bool decompressRLE(const void* compressed, size_t compressedSize, 
                       void* output, size_t outputSize);
    
    Config config_;
    KvPageTier* pageTier_;
    
    std::atomic<bool> running_{false};
    std::vector<std::thread> ioThreads_;
    
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::queue<SwapRequest> requestQueue_;
    
    int swapFd_{-1};
    std::atomic<uint64_t> swapFileOffset_{0};
    
    mutable std::mutex statsMutex_;
    Stats stats_{};
    
    std::atomic<size_t> currentHotUsage_{0};
};

} // namespace splash
