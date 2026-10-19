// Week 2 optimizations test: speculation tuning.
//
// The persistent ANE calibration cache this file also covered is gone: the
// engine keeps this Mac's calibration of the model in the model's own store
// (ane::recall/remember), which ane_ffn_startup_test covers.

#include "engine/SpeculationTuning.hpp"
#include <cassert>
#include <iostream>

namespace splash::engine {
namespace {

void testSpeculationTuning() {
  std::cout << "Testing speculation tuning...\n";

  // Test default threshold
  SpeculationTuning tuning1;
  assert(tuning1.acceptanceThreshold == 0.5f);
  assert(tuning1.enabled == true);

  // Test custom threshold
  SpeculationTuning tuning2 = SpeculationTuning::fromThreshold(0.3f);
  assert(tuning2.acceptanceThreshold == 0.3f);

  // Test clamping
  SpeculationTuning tuning3 = SpeculationTuning::fromThreshold(1.5f);
  assert(tuning3.acceptanceThreshold == 1.0f);

  SpeculationTuning tuning4 = SpeculationTuning::fromThreshold(-0.5f);
  assert(tuning4.acceptanceThreshold == 0.0f);

  std::cout << "  ✓ Speculation tuning threshold validation works\n";
}

} // namespace
} // namespace splash::engine

int main() {
  using namespace splash::engine;

  std::cout << "Running Week 2 optimizations tests...\n\n";

  testSpeculationTuning();

  std::cout << "\n✓ All Week 2 optimization tests passed\n";
  return 0;
}
