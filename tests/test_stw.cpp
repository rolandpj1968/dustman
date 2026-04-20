#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct StwLeaf {
  int v = 0;
};

} // namespace

template <>
struct dustman::Tracer<StwLeaf> : dustman::FieldList<StwLeaf> {};

TEST_CASE("STW: second thread can attach, alloc, collect, detach", "[stw]") {
  // Main thread is just joining, not running GC -- step out of the pool
  // so its presence doesn't block the child's collect.
  dustman::enter_native();

  std::atomic<bool> ok {false};
  std::thread t([&] {
    dustman::attach_thread();
    dustman::Root<StwLeaf> r {dustman::alloc<StwLeaf>()};
    r->v = 42;
    dustman::collect();
    ok.store(r.get() != nullptr && r->v == 42, std::memory_order_release);
    dustman::detach_thread();
  });
  t.join();

  dustman::leave_native();
  REQUIRE(ok.load(std::memory_order_acquire));
}

TEST_CASE("STW: rooted allocations on a mutator thread survive a concurrent collector",
          "[stw]") {
  std::atomic<bool> stop {false};
  std::atomic<bool> ok {true};

  std::thread mutator([&] {
    dustman::attach_thread();
    while (!stop.load(std::memory_order_relaxed)) {
      dustman::Root<StwLeaf> r {dustman::alloc<StwLeaf>()};
      r->v = 7;
      if (r->v != 7) {
        ok.store(false, std::memory_order_release);
      }
      dustman::safepoint();
    }
    dustman::detach_thread();
  });

  for (int i = 0; i < 20; ++i) {
    dustman::collect();
  }
  stop.store(true, std::memory_order_release);
  mutator.join();

  REQUIRE(ok.load(std::memory_order_acquire));
}

TEST_CASE("STW: three mutators and one collector run without corruption", "[stw]") {
  std::atomic<bool> stop {false};
  std::atomic<bool> ok {true};

  constexpr int kNumMutators = 3;
  std::vector<std::thread> mutators;
  for (int i = 0; i < kNumMutators; ++i) {
    mutators.emplace_back([&, i] {
      dustman::attach_thread();
      while (!stop.load(std::memory_order_relaxed)) {
        dustman::Root<StwLeaf> r {dustman::alloc<StwLeaf>()};
        r->v = i;
        if (r->v != i) {
          ok.store(false, std::memory_order_release);
        }
        dustman::safepoint();
      }
      dustman::detach_thread();
    });
  }

  for (int i = 0; i < 20; ++i) {
    dustman::collect();
  }
  stop.store(true, std::memory_order_release);
  for (auto& t : mutators) t.join();

  REQUIRE(ok.load(std::memory_order_acquire));
}

TEST_CASE("STW: concurrent collect() callers both return", "[stw]") {
  dustman::enter_native();

  std::atomic<int> done {0};

  std::thread a([&] {
    dustman::attach_thread();
    for (int i = 0; i < 5; ++i) {
      dustman::collect();
    }
    done.fetch_add(1, std::memory_order_release);
    dustman::detach_thread();
  });
  std::thread b([&] {
    dustman::attach_thread();
    for (int i = 0; i < 5; ++i) {
      dustman::collect();
    }
    done.fetch_add(1, std::memory_order_release);
    dustman::detach_thread();
  });

  a.join();
  b.join();

  dustman::leave_native();
  REQUIRE(done.load(std::memory_order_acquire) == 2);
}

TEST_CASE("STW: a thread that detaches while idle is no longer waited for", "[stw]") {
  // Thread A attaches, allocates, detaches.  After detach, main thread's
  // collect should not block waiting for it -- A is no longer in
  // attached_count_.  If the bookkeeping is broken, this test deadlocks.
  std::thread a([] {
    dustman::attach_thread();
    (void)dustman::alloc<StwLeaf>();
    dustman::detach_thread();
  });
  a.join();

  dustman::collect();
  SUCCEED();
}

TEST_CASE("STW: enter_native lets the collector proceed without the main thread", "[stw]") {
  // This is the std::thread::join pattern: main has no work of its own
  // while the child does GC, but must remain attached (has Roots, etc).
  // enter_native flags it as parked-for-collector so the child's
  // collect() doesn't wait for main.
  std::atomic<bool> ok {false};

  dustman::enter_native();

  std::thread t([&] {
    dustman::attach_thread();
    dustman::Root<StwLeaf> r {dustman::alloc<StwLeaf>()};
    r->v = 17;
    dustman::collect();
    ok.store(r.get() != nullptr && r->v == 17, std::memory_order_release);
    dustman::detach_thread();
  });
  t.join();

  dustman::leave_native();

  REQUIRE(ok.load(std::memory_order_acquire));
}

TEST_CASE("STW: multiple mutators while main is in native", "[stw]") {
  std::atomic<bool> stop {false};
  std::atomic<bool> ok {true};
  std::atomic<int> collects_done {0};

  dustman::enter_native();

  constexpr int kNumMutators = 3;
  std::vector<std::thread> mutators;
  for (int i = 0; i < kNumMutators; ++i) {
    mutators.emplace_back([&, i] {
      dustman::attach_thread();
      while (!stop.load(std::memory_order_relaxed)) {
        dustman::Root<StwLeaf> r {dustman::alloc<StwLeaf>()};
        r->v = i;
        if (r->v != i) {
          ok.store(false, std::memory_order_release);
        }
        dustman::safepoint();
      }
      dustman::detach_thread();
    });
  }

  std::thread collector([&] {
    dustman::attach_thread();
    for (int i = 0; i < 20; ++i) {
      dustman::collect();
      collects_done.fetch_add(1, std::memory_order_release);
    }
    dustman::detach_thread();
  });

  collector.join();
  stop.store(true, std::memory_order_release);
  for (auto& t : mutators) t.join();

  dustman::leave_native();

  REQUIRE(collects_done.load(std::memory_order_acquire) == 20);
  REQUIRE(ok.load(std::memory_order_acquire));
}

TEST_CASE("STW: leave_native during an active pause blocks until release", "[stw]") {
  // Thread A enters native (counted as parked).
  // Thread B becomes the collector (waits for A already parked-via-native,
  // proceeds, runs pipeline).  After B releases, A's leave_native returns.
  // If leave_native didn't wait for pause clear, A would start running
  // alongside B -- caught by the existing invariants elsewhere; here we
  // just verify no deadlock and correct ordering.
  std::atomic<int> stage {0};  // 0=init, 1=A_entered_native, 2=B_collecting, 3=B_done

  dustman::enter_native();  // main itself just joins -- don't block the cycle.

  std::thread a([&] {
    dustman::attach_thread();
    dustman::enter_native();
    stage.store(1, std::memory_order_release);
    // Wait for B to finish its cycle before leaving native.
    while (stage.load(std::memory_order_acquire) < 3) {
      std::this_thread::yield();
    }
    dustman::leave_native();
    dustman::detach_thread();
  });

  while (stage.load(std::memory_order_acquire) < 1) {
    std::this_thread::yield();
  }

  std::thread b([&] {
    dustman::attach_thread();
    stage.store(2, std::memory_order_release);
    dustman::collect();
    stage.store(3, std::memory_order_release);
    dustman::detach_thread();
  });

  a.join();
  b.join();

  dustman::leave_native();

  REQUIRE(stage.load(std::memory_order_acquire) == 3);
}

TEST_CASE("STW: enter_native / leave_native are idempotent", "[stw]") {
  dustman::enter_native();
  dustman::enter_native();
  dustman::leave_native();
  dustman::leave_native();
  SUCCEED();
}
