#include "TestChecks.hpp"
#include "engine/KvDefragmentation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace splash;
using namespace splash::engine;

using splash::test::require;

// A pass is warranted exactly at the policy's threshold, and not below it.
void testShouldDefragmentThreshold() {
  KvDefragmentation defrag(1024);
  require(defrag.shouldDefragment(40.0f), "40% fragmentation was not defragmented");
  require(defrag.shouldDefragment(75.5f), "75.5% fragmentation was not defragmented");
  require(!defrag.shouldDefragment(39.9f), "39.9% fragmentation was defragmented");
  require(!defrag.shouldDefragment(0.0f), "an unfragmented pool was defragmented");
}

// Fragmentation is zero while the pool is empty and while it is full: the
// ratio measures holes among the free pages, so neither state has any.
void testFragmentationOfEmptyAndFullPools() {
  KvDefragmentation empty(64);
  require(empty.getFragmentation() == 0.0f, "an empty pool reported fragmentation");

  KvDefragmentation full(64);
  for (size_t page = 0; page < 64; ++page) full.markUsed(page, 1);
  require(full.getFragmentation() == 0.0f, "a full pool reported fragmentation");
}

// Holes among the free pages are what the ratio measures: freeing every other
// page of a used run leaves the free pages split, so the largest free block is
// smaller than the free page count.
void testFragmentationFromSplitFreePages() {
  KvDefragmentation defrag(16);
  for (size_t page = 0; page < 16; ++page) defrag.markUsed(page, 1);
  for (size_t page = 0; page < 16; page += 2) defrag.markFree(page);

  // 8 free pages in 8 blocks of one: the largest block is 1 of 8.
  const float fragmentation = defrag.getFragmentation();
  require(fragmentation > 0.0f, "split free pages reported no fragmentation");
  require(fragmentation < 1.0f, "split free pages reported total fragmentation");
  require(defrag.needsCompaction(0), "any hole needed no compaction at threshold 0");
  require(!defrag.needsCompaction(100), "a hole needed compaction at threshold 100");
}

// A run of free pages is one block, so it reports no fragmentation.
void testContiguousFreePagesAreNotFragmented() {
  KvDefragmentation defrag(16);
  for (size_t page = 0; page < 8; ++page) defrag.markUsed(page, 1);
  require(defrag.getFragmentation() == 0.0f,
          "a single free run of 8 pages reported fragmentation");
}

// The plan moves used pages down into the holes below them, and names nothing
// that is already in place.
void testCompactionPlanPacksUsedPages() {
  KvDefragmentation defrag(8);
  defrag.markUsed(0, 1);
  defrag.markUsed(2, 1);
  defrag.markUsed(5, 1);

  const std::vector<std::pair<size_t, size_t>> moves = defrag.getCompactionPlan();
  // Page 0 is already at 0; 2 moves to 1; 5 moves to 2.
  require(moves.size() == 2, "the compaction plan did not name both moves");
  require(moves[0].first == 2 && moves[0].second == 1,
          "the first move did not pack page 2 into slot 1");
  require(moves[1].first == 5 && moves[1].second == 2,
          "the second move did not pack page 5 into slot 2");
}

// Marking the same page twice, or freeing one that was never used, does not
// double-count it.
void testRepeatedMarksAreIdempotent() {
  KvDefragmentation defrag(8);
  defrag.markUsed(3, 1);
  defrag.markUsed(3, 1);
  defrag.markFree(3);
  defrag.markFree(3);

  require(defrag.getFragmentation() == 0.0f,
          "repeated marks left the pool fragmented");
  require(defrag.getCompactionPlan().empty(),
          "repeated marks left pages to move");
}

// Out-of-range page ids are ignored rather than corrupting the pool.
void testOutOfRangePagesAreIgnored() {
  KvDefragmentation defrag(4);
  defrag.markUsed(99, 1);
  defrag.markFree(99);
  require(defrag.getFragmentation() == 0.0f,
          "an out-of-range page changed the fragmentation");
}

} // namespace

int main() {
  try {
    testShouldDefragmentThreshold();
    testFragmentationOfEmptyAndFullPools();
    testFragmentationFromSplitFreePages();
    testContiguousFreePagesAreNotFragmented();
    testCompactionPlanPacksUsedPages();
    testRepeatedMarksAreIdempotent();
    testOutOfRangePagesAreIgnored();
    std::cout << "KV defragmentation tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "KV defragmentation test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
