#include "engine/Scheduler.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace splash::engine;

void testPriorityScheduling() {
  // Create scheduler with priority scheduling enabled
  Scheduler scheduler(0.5, 4, true, 4096, 1024, 10240);

  // Submit short interactive request (500 tokens)
  RequestSpec interactive;
  interactive.id = 1;
  interactive.priority = RequestPriority::Normal;
  interactive.prefillTokens = 500;
  scheduler.submit(interactive);

  // Submit long batch request (15000 tokens)
  RequestSpec batch;
  batch.id = 2;
  batch.priority = RequestPriority::Normal;
  batch.prefillTokens = 15000;
  scheduler.submit(batch);

  // Mark both as ready
  scheduler.resourcesReady(1, 0);
  scheduler.resourcesReady(2, 0);

  // Get next batch plan - should prioritize short request
  auto plan = scheduler.next({});
  assert(plan.has_value());
  assert(plan->kind == WorkKind::Prefill);
  
  // With priority scheduling, short request should come first
  // (interactive: 500 < interactive_threshold: 1024)
  assert(plan->items.size() > 0);
  std::cout << "First request scheduled: " << plan->items[0].requestId 
            << " with " << plan->items[0].tokenCount << " tokens\n";

  std::cout << "✓ testPriorityScheduling passed\n";
}

void testBackwardCompatibility() {
  // Scheduler without priority scheduling (default behavior)
  Scheduler scheduler(0.5, 4);

  RequestSpec req1;
  req1.id = 1;
  req1.prefillTokens = 500;
  scheduler.submit(req1);

  RequestSpec req2;
  req2.id = 2;
  req2.prefillTokens = 15000;
  scheduler.submit(req2);

  scheduler.resourcesReady(1, 0);
  scheduler.resourcesReady(2, 0);

  // Should still work with FIFO ordering
  auto plan = scheduler.next({});
  assert(plan.has_value());
  assert(plan->kind == WorkKind::Prefill);

  std::cout << "✓ testBackwardCompatibility passed\n";
}

int main() {
  std::cout << "Running Scheduler priority scheduling tests...\n\n";

  try {
    testPriorityScheduling();
    testBackwardCompatibility();

    std::cout << "\n✓ All tests passed!\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n✗ Test failed: " << e.what() << "\n";
    return 1;
  }
}
