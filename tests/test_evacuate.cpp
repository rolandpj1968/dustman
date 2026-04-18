#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct EvacLeaf {
  int v = 0;
};

struct EvacNode {
  dustman::gc_ptr<EvacLeaf> a;
  int payload = 0;
};

struct EvacCycle {
  dustman::gc_ptr<EvacCycle> next;
  int tag = 0;
};

struct EvacHuge {
  std::uint8_t bulk[8192] {};
  int sentinel = 0;
};

struct ThresholdGuard {
  std::uint32_t saved;
  explicit ThresholdGuard(std::uint32_t t)
      : saved(dustman::get_evacuation_threshold_percent()) {
    dustman::set_evacuation_threshold_percent(t);
  }
  ~ThresholdGuard() {
    dustman::set_evacuation_threshold_percent(saved);
  }
  ThresholdGuard(const ThresholdGuard&) = delete;
  ThresholdGuard& operator=(const ThresholdGuard&) = delete;
};

} // namespace

template <>
struct dustman::Tracer<EvacLeaf> : dustman::FieldList<EvacLeaf> {};

template <>
struct dustman::Tracer<EvacNode> : dustman::FieldList<EvacNode, &EvacNode::a> {};

template <>
struct dustman::Tracer<EvacCycle> : dustman::FieldList<EvacCycle, &EvacCycle::next> {};

template <>
struct dustman::Tracer<EvacHuge> : dustman::FieldList<EvacHuge> {};

TEST_CASE("evacuation relocates a sparsely-live object and updates the root", "[evacuate]") {
  ThresholdGuard g(100);

  dustman::Root<EvacLeaf> r {dustman::alloc<EvacLeaf>()};
  r->v = 42;
  void* old_addr = r.get();

  dustman::collect();

  REQUIRE(r.get() != nullptr);
  REQUIRE(r.get() != old_addr);
  REQUIRE(r->v == 42);
  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE(dustman::detail::is_start(r.get()));
}

TEST_CASE("evacuation updates gc_ptr fields inside evacuated objects", "[evacuate]") {
  ThresholdGuard g(100);

  dustman::Root<EvacNode> r {dustman::alloc<EvacNode>()};
  r->a = dustman::alloc<EvacLeaf>();
  r->a->v = 7;
  r->payload = 123;

  void* old_node = r.get();
  void* old_leaf = r->a.get();

  dustman::collect();

  REQUIRE(r.get() != old_node);
  REQUIRE(r->a.get() != nullptr);
  REQUIRE(r->a.get() != old_leaf);
  REQUIRE(r->payload == 123);
  REQUIRE(r->a->v == 7);
}

TEST_CASE("evacuation of a cyclic graph updates back-edges and terminates", "[evacuate]") {
  ThresholdGuard g(100);

  dustman::Root<EvacCycle> a {dustman::alloc<EvacCycle>()};
  a->next = dustman::alloc<EvacCycle>();
  a->next->next = a;
  a->tag = 1;
  a->next->tag = 2;

  void* old_a = a.get();
  void* old_b = a->next.get();

  dustman::collect();

  REQUIRE(a.get() != old_a);
  REQUIRE(a->next.get() != old_b);
  REQUIRE(a->next.get() != nullptr);
  REQUIRE(a->next->next.get() == a.get());
  REQUIRE(a->tag == 1);
  REQUIRE(a->next->tag == 2);
}

TEST_CASE("two roots aliasing the same object converge after evacuation", "[evacuate]") {
  ThresholdGuard g(100);

  dustman::Root<EvacLeaf> r1 {dustman::alloc<EvacLeaf>()};
  r1->v = 99;
  dustman::gc_ptr<EvacLeaf> alias = r1;
  dustman::Root<EvacLeaf> r2 {alias};

  REQUIRE(r1.get() == r2.get());
  void* before = r1.get();

  dustman::collect();

  REQUIRE(r1.get() == r2.get());
  REQUIRE(r1.get() != before);
  REQUIRE(r1->v == 99);
}

TEST_CASE("dense-block threshold disables evacuation", "[evacuate]") {
  ThresholdGuard g(0);

  dustman::Root<EvacLeaf> r {dustman::alloc<EvacLeaf>()};
  void* before = r.get();

  dustman::collect();

  REQUIRE(r.get() == before);
  REQUIRE(dustman::detail::is_marked(r.get()));
}

TEST_CASE("huge object is not evacuated even at 100% threshold", "[evacuate]") {
  ThresholdGuard g(100);

  dustman::Root<EvacHuge> r {dustman::alloc<EvacHuge>()};
  r->sentinel = 0xBEEF;
  void* before = r.get();

  dustman::collect();

  REQUIRE(r.get() == before);
  REQUIRE(r->sentinel == 0xBEEF);
}

TEST_CASE("consecutive collects with evacuation: survivor stays consistent", "[evacuate]") {
  // Shape of the phase 3b-ii bug: recycle list populated by one collect,
  // blocks then flagged for evacuation in the next. Removing the
  // small_recycle_.clear() in classify_and_destroy_dead would let
  // alloc_slow_small pop a flagged block as an evacuation target,
  // causing set_start/set_mark on the copy to land in the source's
  // bitmaps and producing a self-iterating cascade that ends in chained
  // forwarding headers. This test exercises the shape; any resulting
  // crash or data corruption fails it.
  ThresholdGuard g(50);

  dustman::Root<EvacLeaf> survivor {dustman::alloc<EvacLeaf>()};
  survivor->v = 1;

  for (int i = 0; i < 20; ++i) {
    (void)dustman::alloc<EvacLeaf>();
  }
  dustman::collect();
  REQUIRE(survivor->v == 1);

  for (int i = 0; i < 20; ++i) {
    (void)dustman::alloc<EvacLeaf>();
  }
  dustman::collect();
  REQUIRE(survivor->v == 1);

  dustman::collect();
  REQUIRE(survivor->v == 1);
}
