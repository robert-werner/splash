#include "TestChecks.hpp"
#include "engine/LazyAneAllocation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using namespace splash;
using namespace splash::engine;

using splash::test::require;

// The defaults defer allocation, hold nothing back until tokens arrive, and
// release the ANE after five idle minutes.
void testPolicyDefaults() {
  LazyAnePolicy policy;
  require(policy.enabled, "lazy allocation is off by default");
  require(policy.reserveOnly, "the default policy does not reserve only");
  require(policy.minTokensBeforeAllocation == 0,
          "the default policy defers past zero tokens");
  require(policy.idleReleaseMs == 300000.0,
          "the default idle release is not five minutes");
  require(policy.trackUsage, "the default policy does not track usage");
}

// A fresh allocator holds nothing.
void testInitialState() {
  LazyAneAllocator allocator(LazyAnePolicy{});
  const LazyAneStats &stats = allocator.stats();
  require(!stats.allocated, "a fresh allocator reports the ANE allocated");
  require(!allocator.isAllocated(), "a fresh allocator is allocated");
  require(stats.allocations == 0 && stats.releases == 0,
          "a fresh allocator counted an allocation or release");
  require(stats.tokensProcessed == 0 && stats.memorySavedBytes == 0,
          "a fresh allocator counted tokens or saved memory");
}

// With no minimum, the first call allocates; once allocated it does not ask
// again.
void testAllocateImmediatelyThenHold() {
  LazyAneAllocator allocator(LazyAnePolicy{});
  require(allocator.shouldAllocate(0, 0.0), "allocation was deferred past zero tokens");
  allocator.markAllocated(0.0, 4096);
  require(allocator.isAllocated(), "the ANE was not marked allocated");
  require(!allocator.shouldAllocate(1, 1.0), "an allocated ANE was allocated again");
  require(allocator.stats().allocations == 1, "the allocation was not counted");
}

// A minimum defers allocation until that many tokens have been processed.
void testMinimumTokensDefers() {
  LazyAnePolicy policy;
  policy.minTokensBeforeAllocation = 64;
  LazyAneAllocator allocator(policy);
  require(!allocator.shouldAllocate(63, 0.0), "allocation happened below the minimum");
  require(allocator.shouldAllocate(64, 0.0), "allocation did not happen at the minimum");
}

// With lazy allocation off, the allocator asks for the ANE straight away.
void testDisabledLazyAllocatesImmediately() {
  LazyAnePolicy policy;
  policy.enabled = false;
  LazyAneAllocator allocator(policy);
  require(allocator.shouldAllocate(0, 0.0),
          "a non-lazy allocator deferred allocation");
}

// The ANE is released once it has been idle for the policy's window, and not
// before it.
void testIdleRelease() {
  LazyAneAllocator allocator(LazyAnePolicy{});
  allocator.markAllocated(0.0, 4096);
  allocator.recordUsage(10, 0.0);
  require(!allocator.shouldRelease(299999.0), "the ANE was released early");
  require(allocator.shouldRelease(300000.0), "the ANE was not released when idle");

  allocator.markReleased(300000.0);
  require(!allocator.isAllocated(), "the ANE was not marked released");
  require(allocator.stats().releases == 1, "the release was not counted");
  require(!allocator.shouldRelease(400000.0), "a released ANE was released again");
}

// Usage extends the idle window.
void testUsageExtendsIdleWindow() {
  LazyAneAllocator allocator(LazyAnePolicy{});
  allocator.markAllocated(0.0, 4096);
  allocator.recordUsage(10, 200000.0);
  require(!allocator.shouldRelease(400000.0),
          "usage did not extend the idle window");
  require(allocator.shouldRelease(500000.0),
          "the ANE was not released after the extended window");
  require(allocator.stats().tokensProcessed == 10, "usage was not counted");
}

// A policy that does not track usage never releases on idle.
void testUntrackedUsageNeverReleases() {
  LazyAnePolicy policy;
  policy.trackUsage = false;
  LazyAneAllocator allocator(policy);
  allocator.markAllocated(0.0, 4096);
  require(!allocator.shouldRelease(1e12), "an untracked ANE was released");
}

// Releasing an ANE that was never allocated is not a release.
void testReleaseWithoutAllocationIsIgnored() {
  LazyAneAllocator allocator(LazyAnePolicy{});
  allocator.markReleased(0.0);
  require(allocator.stats().releases == 0, "a spurious release was counted");
}

} // namespace

int main() {
  try {
    testPolicyDefaults();
    testInitialState();
    testAllocateImmediatelyThenHold();
    testMinimumTokensDefers();
    testDisabledLazyAllocatesImmediately();
    testIdleRelease();
    testUsageExtendsIdleWindow();
    testUntrackedUsageNeverReleases();
    testReleaseWithoutAllocationIsIgnored();
    std::cout << "lazy ANE allocation tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "lazy ANE allocation test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
