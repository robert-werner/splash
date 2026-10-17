#include "TestChecks.hpp"
#include "TestKvPool.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using splash::engine::KvPool;
using splash::test::TestKvStorage;

using splash::test::require;

void release(KvPool &pool, const std::vector<uint32_t> &pages) {
  for (uint32_t page : pages) pool.releasePage(page, false);
}

// Holds every page, then frees the ones named, so a test can shape the free
// pages across extents. Every pool here has 4 pages per extent.
KvPool held(TestKvStorage &storage, const std::vector<uint32_t> &released) {
  KvPool pool(storage, storage.pageCount());
  auto all = pool.acquirePages(storage.pageCount());
  require(all.granted(), "fragmentation setup did not acquire every page");
  release(pool, released);
  return pool;
}

// A pool holding every page has no free page to strand, and a pool whose free
// pages fill whole extents releases them without moving anything. Neither is
// fragmented, so neither warrants a compaction pass.
void testWholeExtentsAreNotFragmentation() {
  TestKvStorage all(12, 100, 4);
  KvPool none = held(all, {});
  require(none.currentFragmentationRatio() == 0,
          "a pool holding every page reported fragmentation");

  // Pages 0-3 and 4-7 are two whole extents; nothing is stranded.
  TestKvStorage whole(12, 100, 4);
  KvPool pool = held(whole, {0, 1, 2, 3, 4, 5, 6, 7});
  require(pool.currentFragmentationRatio() == 0,
          "free whole extents were counted as fragmentation");
}

// A free page whose extent peers are still held cannot be released on its
// own. That stranding is what a compaction pass exists to undo, so it is what
// the ratio reports.
void testStrandedPagesAreFragmentation() {
  // One page free in each of three extents: 3 of 12 pages = 25%.
  TestKvStorage storage(12, 100, 4);
  KvPool pool = held(storage, {0, 4, 8});
  require(pool.currentFragmentationRatio() == 25,
          "three stranded pages of twelve did not report 25%");
}

// Only the extents in use strand pages, so a whole free extent alongside a
// partly used one reports just the partly used one's free pages.
void testWholeExtentIsExcludedFromTheRatio() {
  // Extent 0 free whole (0-3); extent 1 strands two (4,5); extent 2 held.
  TestKvStorage storage(12, 100, 4);
  KvPool pool = held(storage, {0, 1, 2, 3, 4, 5});
  require(pool.currentFragmentationRatio() == 16,
          "a whole free extent changed the stranded ratio");
}

// Compaction is what clears the stranding, and the ratio follows it: a pass
// that moves nothing leaves the ratio alone, and one that empties an extent
// lowers it.
void testCompactionLowersFragmentation() {
  TestKvStorage storage(12, 100, 4);
  KvPool pool = held(storage, {0, 4, 8});
  const uint32_t before = pool.currentFragmentationRatio();
  require(before == 25, "the setup did not strand three pages");

  // Compaction empties an extent only once the free pages of the extents in
  // use cover it: three free pages do not cover an extent of four.
  const auto refused = pool.compactExtent({});
  require(refused.empty(), "three free pages emptied an extent of four");
  require(pool.currentFragmentationRatio() == before,
          "a pass that moved nothing changed the ratio");

  // A fourth free page covers an extent, so the pass empties one and the
  // stranded pages become a whole extent the pool can release.
  release(pool, {9});
  const auto moved = pool.compactExtent({});
  require(!moved.empty(), "four free pages emptied no extent");
  require(pool.currentFragmentationRatio() < 33,
          "compaction left the pages as stranded as it found them");
}

} // namespace

int main() {
  try {
    testWholeExtentsAreNotFragmentation();
    testStrandedPagesAreFragmentation();
    testWholeExtentIsExcludedFromTheRatio();
    testCompactionLowersFragmentation();
    std::cout << "KV defragmentation tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "KV defragmentation test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
