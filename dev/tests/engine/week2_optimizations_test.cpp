// Week 2 optimizations test: fused attention, persistent ANE calibration, speculation tuning

#include "engine/AneFfnPersistentCache.hpp"
#include "engine/SpeculationTuning.hpp"
#include <cassert>
#include <iostream>

namespace splash::engine {
namespace {

void testPersistentCache() {
  std::cout << "Testing ANE persistent calibration cache...\n";
  
  AneFfnPersistentCache cache;
  
  // Test save and load
  ops::ane_ffn::Calibration cal;
  cal.aneUnits = 16;
  cal.minimumRows = 256;
  cal.gpu[0] = {256, 2.5};
  cal.gpu[1] = {512, 4.8};
  
  cache.save("test_model", 16, cal);
  
  auto loaded = cache.load("test_model", 16);
  assert(loaded.has_value());
  assert(loaded->aneUnits == 16);
  assert(loaded->minimumRows == 256);
  
  // Test removal
  cache.remove("test_model", 16);
  auto removed = cache.load("test_model", 16);
  assert(!removed.has_value());
  
  std::cout << "  ✓ Persistent cache save/load/remove works\n";
}

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
  
  testPersistentCache();
  testSpeculationTuning();
  
  std::cout << "\n✓ All Week 2 optimization tests passed\n";
  return 0;
}
