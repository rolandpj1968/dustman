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
  dustman::detach_thread();

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

  dustman::attach_thread();
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
  dustman::detach_thread();

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

  dustman::attach_thread();
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
