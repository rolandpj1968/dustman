// Pre-test cleanup listener: drain orphan allocations left by prior
// tests before each TEST_CASE runs.  dustman has one process-wide Heap
// singleton and Catch2 runs tests sequentially on a single thread; any
// unrooted allocation in an earlier test persists until some later
// test's collect() sweeps it, which confounds count-based assertions.
//
// This does NOT give full isolation -- it does not reset block layout
// or the recycle list, and it does not help with stale gc_ptr<T>s
// held across a test's own collect() (which is a separate UAF class
// handled per-test).

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "dustman/collect.hpp"

namespace {

class DrainBetweenTests : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseStarting(Catch::TestCaseInfo const&) override {
    dustman::collect();
  }
};

} // namespace

CATCH_REGISTER_LISTENER(DrainBetweenTests)
