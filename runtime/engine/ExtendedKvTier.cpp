#include "ExtendedKvTier.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

namespace splash::engine {

struct ExtendedKvTier::Transfer final {
  bool demotion = false;
  std::shared_ptr<model::SlotFile::Operation> io;
  bool ready = false;
  bool success = false;
};

class ExtendedKvTier::Ticket final : public KvTransfer {
public:
  explicit Ticket(std::shared_ptr<Transfer> transfer) : transfer_(std::move(transfer)) {}
  [[nodiscard]] bool ready() const noexcept override { return transfer_->ready; }
  [[nodiscard]] bool finish() override { return transfer_->success; }
private:
  std::shared_ptr<Transfer> transfer_;
};

namespace {
std::shared_ptr<ExtendedKvTier::DiskSlot> diskSlot(const std::shared_ptr<KvDiskSlot> &slot) {
  auto disk = std::dynamic_pointer_cast<ExtendedKvTier::DiskSlot>(slot);
  if (!disk)
    throw std::invalid_argument("KV disk slot belongs to another tier");
  return disk;
}
} // namespace

ExtendedKvTier::ExtendedKvTier(kv::PageStorage &pages,
                               std::shared_ptr<model::SlotFile> hotFile,
                               std::shared_ptr<model::SlotFile> coldFile,
                               const Config &config)
    : pages_(pages), hotFile_(std::move(hotFile)), coldFile_(std::move(coldFile)),
      config_(config), transferLimit_(128),
      demotionLimit_(transferLimit_ / 2), restoreLimit_(transferLimit_ * 3 / 4) {
  if (!hotFile_ || !transferLimit_)
    throw std::invalid_argument("Extended KV tier needs files and transfer room");
  inFlight_.reserve(transferLimit_);
}

ExtendedKvTier::~ExtendedKvTier() {
  for (const std::shared_ptr<Transfer> &transfer : inFlight_)
    if (transfer->io) transfer->io->drain();
}

uint64_t ExtendedKvTier::slotBytes() const noexcept { return hotFile_->slotBytes(); }
bool ExtendedKvTier::writable() const noexcept {
  return hotFile_->writable() && (!coldFile_ || coldFile_->writable());
}
bool ExtendedKvTier::persistent() const noexcept { return hotFile_->persistent(); }

void ExtendedKvTier::label(const std::shared_ptr<KvDiskSlot> &slot, std::vector<std::byte> label) {
  auto disk = diskSlot(slot);
  if (disk->inColdTier && disk->coldSlot)
    coldFile_->label(disk->coldSlot, std::move(label));
  else if (disk->hotSlot)
    hotFile_->label(disk->hotSlot, std::move(label));
}

std::shared_ptr<KvDiskSlot> ExtendedKvTier::adopt(const model::SlotRecord &record) {
  if (auto hotSlot = hotFile_->adopt(record))
    return std::make_shared<DiskSlot>(std::move(hotSlot));
  if (coldFile_) {
    if (auto coldSlot = coldFile_->adopt(record)) {
      auto slot = std::make_shared<DiskSlot>(nullptr, std::move(coldSlot));
      slot->inColdTier = true;
      return slot;
    }
  }
  return nullptr;
}

bool ExtendedKvTier::canDemote() const noexcept {
  return demotions_ < demotionLimit_ && inFlight_.size() < transferLimit_ && writable();
}

bool ExtendedKvTier::canRestore() const noexcept {
  return restores_ < restoreLimit_ && inFlight_.size() < transferLimit_;
}

std::shared_ptr<KvDiskSlot> ExtendedKvTier::acquireSlot() {
  if (auto hotSlot = hotFile_->acquire())
    return std::make_shared<DiskSlot>(std::move(hotSlot));
  if (coldFile_) {
    if (auto coldSlot = coldFile_->acquire()) {
      auto slot = std::make_shared<DiskSlot>(nullptr, std::move(coldSlot));
      slot->inColdTier = true;
      return slot;
    }
  }
  return nullptr;
}

std::unique_ptr<KvTransfer>
ExtendedKvTier::demote(uint32_t page, std::shared_ptr<KvDiskSlot> slot,
                       std::function<void()> completion) {
  if (!canDemote()) return {};
  auto disk = diskSlot(slot);
  auto transfer = std::make_shared<Transfer>();
  transfer->demotion = true;
  auto ticket = std::make_unique<Ticket>(transfer);
  
  const std::vector<std::span<std::byte>> bytes = pages_.spans(page);
  if (disk->inColdTier && disk->coldSlot) {
    transfer->io = coldFile_->write(disk->coldSlot, {bytes.begin(), bytes.end()}, std::move(completion));
    ++stats_.demotions;
  } else if (disk->hotSlot) {
    transfer->io = hotFile_->write(disk->hotSlot, {bytes.begin(), bytes.end()}, std::move(completion));
  }
  if (!transfer->io) return {};
  ++demotions_;
  inFlight_.push_back(std::move(transfer));
  return ticket;
}

std::unique_ptr<KvTransfer>
ExtendedKvTier::restore(std::shared_ptr<KvDiskSlot> slot, uint32_t page,
                        std::function<void()> completion) {
  if (!canRestore()) return {};
  auto disk = diskSlot(slot);
  auto transfer = std::make_shared<Transfer>();
  auto ticket = std::make_unique<Ticket>(transfer);
  
  if (disk->inColdTier && disk->coldSlot) {
    transfer->io = coldFile_->read(disk->coldSlot, pages_.spans(page), std::move(completion));
    ++stats_.coldHits;
    ++stats_.pageFaults;
  } else if (disk->hotSlot) {
    transfer->io = hotFile_->read(disk->hotSlot, pages_.spans(page), std::move(completion));
    ++stats_.hotHits;
  }
  if (!transfer->io) return {};
  ++restores_;
  inFlight_.push_back(std::move(transfer));
  return ticket;
}

void ExtendedKvTier::poll() {
  hotFile_->poll();
  if (coldFile_) coldFile_->poll();
  
  for (size_t index = 0; index < inFlight_.size();) {
    if (!inFlight_[index]->io->ready()) {
      ++index;
      continue;
    }
    const std::shared_ptr<Transfer> transfer = std::move(inFlight_[index]);
    inFlight_.erase(inFlight_.begin() + static_cast<std::ptrdiff_t>(index));
    --(transfer->demotion ? demotions_ : restores_);
    transfer->success = transfer->io->wait();
    transfer->io.reset();
    transfer->ready = true;
  }
}

void ExtendedKvTier::prefetchSequential(uint64_t startBlock, uint32_t pageCount) {
  for (uint32_t i = 0; i < pageCount && i < config_.prefetchPages; ++i) {
    uint64_t blockId = startBlock + i;
    if (prefetching_.contains(blockId)) continue;
    PrefetchRequest req{blockId, 0, nullptr, accessCounter_++};
    prefetchQueue_.push_back(std::move(req));
    prefetching_[blockId] = true;
  }
}

bool ExtendedKvTier::promoteToHot(const std::shared_ptr<KvDiskSlot> &slot) {
  auto disk = diskSlot(slot);
  if (!disk->inColdTier) return false;
  if (!hotFile_->acquire()) return false;
  ++stats_.promotions;
  return true;
}

bool ExtendedKvTier::demoteToCold(const std::shared_ptr<KvDiskSlot> &slot) {
  auto disk = diskSlot(slot);
  if (disk->inColdTier) return false;
  if (!coldFile_ || !coldFile_->acquire()) return false;
  disk->inColdTier = true;
  ++stats_.demotions;
  return true;
}

void ExtendedKvTier::recordAccess(uint64_t blockId, uint32_t page) {
  accessHistory_.push_back(blockId);
  lastAccess_[blockId] = accessCounter_++;
  if (accessHistory_.size() > 1000) {
    uint64_t oldest = accessHistory_.front();
    accessHistory_.pop_front();
    lastAccess_.erase(oldest);
  }
  if (isSequentialAccess(blockId)) {
    prefetchSequential(blockId + 1, config_.prefetchPages);
    ++stats_.prefetchHits;
  }
}

bool ExtendedKvTier::isSequentialAccess(uint64_t blockId) const {
  if (accessHistory_.size() < 3) return false;
  auto it = accessHistory_.rbegin();
  uint64_t current = *it++;
  if (current != blockId) return false;
  uint64_t prev1 = *it++;
  uint64_t prev2 = *it;
  return (current == prev1 + 1) && (prev1 == prev2 + 1);
}

std::optional<std::shared_ptr<KvDiskSlot>> ExtendedKvTier::evictFromHot() {
  uint64_t oldestBlock = 0;
  uint64_t oldestAccess = UINT64_MAX;
  for (const auto &[block, access] : lastAccess_) {
    if (access < oldestAccess) {
      oldestAccess = access;
      oldestBlock = block;
    }
  }
  if (oldestBlock == 0) return std::nullopt;
  return std::nullopt;
}

} // namespace splash::engine
