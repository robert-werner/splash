#include "TestChecks.hpp"
#include "engine/WriteCoalescing.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace splash;
using namespace splash::engine;

using splash::test::require;

std::vector<std::byte> bytes(size_t count, std::byte value) {
  return std::vector<std::byte>(count, value);
}

// The defaults describe a 4 MB buffer that coalesces 8 to 64 pages at a time.
void testConfigDefaults() {
  WriteCoalescingConfig config;
  require(config.bufferSizeBytes == 4 * 1024 * 1024, "the buffer is not 4 MB by default");
  require(config.maxCoalesceTimeMs == 50.0, "the coalescing window is not 50 ms by default");
  require(config.minPagesToCoalesce == 8, "the minimum batch is not 8 pages by default");
  require(config.maxPagesToCoalesce == 64, "the maximum batch is not 64 pages by default");
  require(config.enableSequentialCombining, "sequential combining is off by default");
}

// A fresh buffer holds nothing and has counted nothing.
void testInitialState() {
  WriteCoalescingBuffer buffer(WriteCoalescingConfig{});
  const WriteCoalescingStats &stats = buffer.stats();
  require(stats.logicalWrites == 0 && stats.physicalWrites == 0,
          "a fresh buffer counted a write");
  require(stats.logicalBytesWritten == 0 && stats.physicalBytesWritten == 0,
          "a fresh buffer counted bytes");
  require(stats.flushes == 0 && stats.bufferedPages == 0,
          "a fresh buffer counted a flush or a buffered page");
}

// A buffered write is counted as logical and held, not written.
void testAddWriteBuffers() {
  WriteCoalescingBuffer buffer(WriteCoalescingConfig{});
  require(buffer.addWrite(7, 0, bytes(128, std::byte{1}), [] {}),
          "a small write was not buffered");
  const WriteCoalescingStats &stats = buffer.stats();
  require(stats.logicalWrites == 1, "the buffered write was not counted");
  require(stats.logicalBytesWritten == 128, "the buffered bytes were not counted");
  require(stats.bufferedPages == 1, "the buffer does not hold the page");
  require(stats.physicalWrites == 0, "a buffered write reached the disk");
  require(stats.flushes == 0, "buffering a write flushed the buffer");
}

// A write larger than the whole buffer is refused rather than buffered.
void testOversizedWriteIsRefused() {
  WriteCoalescingConfig config;
  config.bufferSizeBytes = 256;
  WriteCoalescingBuffer buffer(config);
  require(!buffer.addWrite(0, 0, bytes(512, std::byte{1}), [] {}),
          "a write larger than the buffer was accepted");
  require(buffer.stats().bufferedPages == 0, "a refused write was buffered");
}

// The buffer refuses a write once it holds its maximum page count.
void testBufferRefusesPastMaximumPages() {
  WriteCoalescingConfig config;
  config.maxPagesToCoalesce = 3;
  config.minPagesToCoalesce = 100; // keep shouldFlush out of the way
  WriteCoalescingBuffer buffer(config);
  for (uint32_t page = 0; page < 3; ++page) {
    require(buffer.addWrite(page, 0, bytes(8, std::byte{1}), [] {}),
            "a write below the page maximum was refused");
  }
  require(!buffer.addWrite(3, 0, bytes(8, std::byte{1}), [] {}),
          "a write past the page maximum was accepted");
  require(buffer.stats().bufferedPages == 3, "the buffer holds the wrong page count");
}

// An empty buffer never asks to be flushed.
void testEmptyBufferDoesNotFlush() {
  WriteCoalescingBuffer buffer(WriteCoalescingConfig{});
  require(!buffer.shouldFlush(1000.0), "an empty buffer asked to be flushed");
  require(buffer.flushIfNeeded(1000.0, [](std::vector<PendingWrite> &) {}) == 0,
          "an empty buffer flushed");
}

// The buffer asks to flush once it holds the policy's minimum page count.
void testFlushAtMinimumPages() {
  WriteCoalescingConfig config;
  config.minPagesToCoalesce = 3;
  WriteCoalescingBuffer buffer(config);
  buffer.addWrite(0, 0, bytes(8, std::byte{1}), [] {});
  buffer.addWrite(1, 8, bytes(8, std::byte{1}), [] {});
  require(!buffer.shouldFlush(0.0), "the buffer flushed below the minimum");
  buffer.addWrite(2, 16, bytes(8, std::byte{1}), [] {});
  require(buffer.shouldFlush(0.0), "the buffer did not flush at the minimum");
}

// A nearly full buffer asks to flush before it holds the minimum page count.
void testFlushWhenNearlyFull() {
  WriteCoalescingConfig config;
  config.bufferSizeBytes = 1000;
  config.minPagesToCoalesce = 100; // the page count cannot trigger the flush
  WriteCoalescingBuffer buffer(config);
  require(buffer.addWrite(0, 0, bytes(900, std::byte{1}), [] {}),
          "a write that fits was refused");
  require(buffer.shouldFlush(0.0), "a buffer at 90% did not ask to flush");
}

// Flushing hands the batch to the writer, clears the buffer and counts the
// flush.
void testForceFlushHandsOverAndClears() {
  WriteCoalescingConfig config;
  config.enableSequentialCombining = false;
  WriteCoalescingBuffer buffer(config);
  buffer.addWrite(4, 0, bytes(16, std::byte{2}), [] {});
  buffer.addWrite(9, 64, bytes(16, std::byte{2}), [] {});

  std::vector<PendingWrite> flushed;
  const uint32_t count = buffer.forceFlush([&](std::vector<PendingWrite> &writes) {
    flushed = writes;
  });

  require(count == 2, "the flush did not report both writes");
  require(flushed.size() == 2, "the writer did not receive both writes");
  require(flushed[0].page == 4 && flushed[1].page == 9,
          "the writer received the wrong pages");
  require(buffer.stats().flushes == 1, "the flush was not counted");
  require(buffer.stats().physicalWrites == 2, "the physical writes were not counted");
  require(buffer.stats().physicalBytesWritten == 32, "the physical bytes were not counted");
  require(buffer.stats().bufferedPages == 0, "the buffer was not cleared");
  require(buffer.forceFlush([&](std::vector<PendingWrite> &) {}) == 0,
          "an emptied buffer flushed again");
}

// Adjacent writes combine into one physical write, and both completions run.
void testSequentialWritesCombine() {
  WriteCoalescingBuffer buffer(WriteCoalescingConfig{});
  bool first = false;
  bool second = false;
  buffer.addWrite(0, 0, bytes(100, std::byte{1}), [&] { first = true; });
  buffer.addWrite(1, 100, bytes(100, std::byte{2}), [&] { second = true; });

  std::vector<PendingWrite> flushed;
  const uint32_t count = buffer.forceFlush([&](std::vector<PendingWrite> &writes) {
    flushed = writes;
  });

  require(count == 1, "adjacent writes were not combined into one");
  require(flushed.size() == 1 && flushed[0].data.size() == 200,
          "the combined write does not hold both payloads");
  require(buffer.stats().logicalWrites == 2 && buffer.stats().physicalWrites == 1,
          "the coalescing ratio was not recorded");
  require(buffer.stats().coalescingRatio() == 0.5,
          "the coalescing ratio is not 0.5");
  require(flushed[0].completion != nullptr, "the combined write lost its completion");
  flushed[0].completion();
  require(first && second, "combining did not run both completions");
}

// Writes that are not adjacent stay separate, whatever order they arrived in.
void testNonAdjacentWritesStaySeparate() {
  WriteCoalescingBuffer buffer(WriteCoalescingConfig{});
  buffer.addWrite(1, 4096, bytes(100, std::byte{1}), [] {});
  buffer.addWrite(0, 0, bytes(100, std::byte{1}), [] {});

  std::vector<PendingWrite> flushed;
  const uint32_t count = buffer.forceFlush([&](std::vector<PendingWrite> &writes) {
    flushed = writes;
  });

  require(count == 2, "non-adjacent writes were combined");
  require(flushed.size() == 2, "the writer received the wrong write count");
  // The buffer sorts by slot offset before writing.
  require(flushed[0].slotOffset == 0 && flushed[1].slotOffset == 4096,
          "the writes were not ordered by slot offset");
}

// Combining off leaves every write as its own physical write.
void testCombiningOffKeepsWritesSeparate() {
  WriteCoalescingConfig config;
  config.enableSequentialCombining = false;
  WriteCoalescingBuffer buffer(config);
  buffer.addWrite(0, 0, bytes(100, std::byte{1}), [] {});
  buffer.addWrite(1, 100, bytes(100, std::byte{1}), [] {});

  const uint32_t count = buffer.forceFlush([](std::vector<PendingWrite> &) {});
  require(count == 2, "writes were combined with combining off");
}

} // namespace

int main() {
  try {
    testConfigDefaults();
    testInitialState();
    testAddWriteBuffers();
    testOversizedWriteIsRefused();
    testBufferRefusesPastMaximumPages();
    testEmptyBufferDoesNotFlush();
    testFlushAtMinimumPages();
    testFlushWhenNearlyFull();
    testForceFlushHandsOverAndClears();
    testSequentialWritesCombine();
    testNonAdjacentWritesStaySeparate();
    testCombiningOffKeepsWritesSeparate();
    std::cout << "write coalescing tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "write coalescing test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
