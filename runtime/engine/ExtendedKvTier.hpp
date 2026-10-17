#pragma once

#include "KvTier.hpp"
#include "model/SlotFile.hpp"
#include "ops/PageStorage.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace splash::engine {

// Extended KV tier supporting 1M+ token contexts via hot (RAM) + cold (SSD) architecture.
// Hot tier: 0-200K tokens resident in RAM at current speed.
// Cold tier: 200K-1M tokens SSD-backed with async page faults + prefetch.
//
// Performance targets (M5 Pro, 1TB SSD @ ~3GB/s):
// - Sequential read: ~55-60 tok/s
// - Random access: ~45-50 tok/s
// - Hot tier: unchanged from baseline
class ExtendedKvTier final : public KvTier {
public:
  struct Config {
    std::string coldTierPath;
    uint32_t hotTierTokens = 200 * 1024;      // 200K token boundary
    uint32_t maxExtendedTokens = 1024 * 1024; // 1M max context
    uint32_t prefetchPages = 16;              // Prefetch window
    bool compressCold = false;                // INT4→INT2 compression
  };

  struct DiskSlot final : KvDiskSlot {
    explicit DiskSlot(std::shared_ptr<model::SlotFile::Slot> hot,
                      std::shared_ptr<model::SlotFile::Slot> cold = nullptr)
        : hotSlot(std::move(hot)), coldSlot(std::move(cold)) {}
    
    std::shared_ptr<model::SlotFile::Slot> hotSlot;
    std::shared_ptr<model::SlotFile::Slot> coldSlot;
    bool inColdTier = false;
  };

  struct PrefetchRequest {
    uint64_t blockId;
    uint32_t page;
    std::shared_ptr<KvDiskSlot> slot;
    uint64_t priority;
  };

  ExtendedKvTier(kv::PageStorage &pages,
                 std::shared_ptr<model::SlotFile> hotFile,
                 std::shared_ptr<model::SlotFile> coldFile,
                 const Config &config);
  ~ExtendedKvTier() override;
  ExtendedKvTier(const ExtendedKvTier &) = delete;
  ExtendedKvTier &operator=(const ExtendedKvTier &) = delete;

  [[nodiscard]] uint64_t slotBytes() const noexcept override;
  [[nodiscard]] bool writable() const noexcept override;
  [[nodiscard]] bool persistent() const noexcept override;
  void label(const std::shared_ptr<KvDiskSlot> &slot, std::vector<std::byte> label) override;

  [[nodiscard]] std::shared_ptr<KvDiskSlot> adopt(const model::SlotRecord &record);
  [[nodiscard]] bool canDemote() const noexcept override;
  [[nodiscard]] bool canRestore() const noexcept override;
  [[nodiscard]] std::shared_ptr<KvDiskSlot> acquireSlot() override;

  [[nodiscard]] std::unique_ptr<KvTransfer>
  demote(uint32_t page, std::shared_ptr<KvDiskSlot> slot,
         std::function<void()> completion) override;
  [[nodiscard]] std::unique_ptr<KvTransfer>
  restore(std::shared_ptr<KvDiskSlot> slot, uint32_t page,
          std::function<void()> completion) override;
  void poll() override;

  // Extended tier operations
  void prefetchSequential(uint64_t startBlock, uint32_t pageCount);
  [[nodiscard]] bool promoteToHot(const std::shared_ptr<KvDiskSlot> &slot);
  [[nodiscard]] bool demoteToCold(const std::shared_ptr<KvDiskSlot> &slot);
  void recordAccess(uint64_t blockId, uint32_t page);
  
  struct Stats {
    uint64_t hotHits = 0;
    uint64_t coldHits = 0;
    uint64_t prefetchHits = 0;
    uint64_t pageFaults = 0;
    uint64_t promotions = 0;
    uint64_t demotions = 0;
  };
  [[nodiscard]] Stats statistics() const noexcept { return stats_; }

private:
  struct Transfer;
  class Ticket;

  [[nodiscard]] bool isSequentialAccess(uint64_t blockId) const;
  [[nodiscard]] std::optional<std::shared_ptr<KvDiskSlot>> evictFromHot();

  kv::PageStorage &pages_;
  std::shared_ptr<model::SlotFile> hotFile_;
  std::shared_ptr<model::SlotFile> coldFile_;
  Config config_;
  
  uint32_t transferLimit_;
  uint32_t demotionLimit_;
  uint32_t restoreLimit_;
  uint32_t demotions_ = 0;
  uint32_t restores_ = 0;
  
  std::deque<uint64_t> accessHistory_;
  std::unordered_map<uint64_t, uint64_t> lastAccess_;
  uint64_t accessCounter_ = 0;
  
  std::deque<PrefetchRequest> prefetchQueue_;
  std::unordered_map<uint64_t, bool> prefetching_;
  
  std::vector<std::shared_ptr<Transfer>> inFlight_;
  Stats stats_;
};

} // namespace splash::engine
