#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct Leaf {
  int v = 0;
};

struct Node {
  dustman::gc_ptr<Leaf> a;
  dustman::gc_ptr<Leaf> b;
  int payload = 0;
};

struct Cyclic {
  dustman::gc_ptr<Cyclic> next;
};

struct BadlyTraced {
  dustman::gc_ptr<Leaf> traced;
  dustman::gc_ptr<Leaf> untraced;
};

// Threshold=0 disables evacuation so the pre-evacuation semantics these
// tests rely on (unmarked objects stay at their original address) hold.
struct NoEvacGuard {
  std::uint32_t saved;
  NoEvacGuard() : saved(dustman::get_evacuation_threshold_percent()) {
    dustman::set_evacuation_threshold_percent(0);
  }
  ~NoEvacGuard() {
    dustman::set_evacuation_threshold_percent(saved);
  }
};

} // namespace

template <>
struct dustman::Tracer<Leaf> : dustman::FieldList<Leaf> {};

template <>
struct dustman::Tracer<Node> : dustman::FieldList<Node, &Node::a, &Node::b> {};

template <>
struct dustman::Tracer<Cyclic> : dustman::FieldList<Cyclic, &Cyclic::next> {};

template <>
struct dustman::Tracer<BadlyTraced> : dustman::FieldList<BadlyTraced, &BadlyTraced::traced> {};

TEST_CASE("alloc sets the start bit for the allocated slot", "[collect]") {
  auto p = dustman::alloc<Leaf>();
  REQUIRE(dustman::detail::is_start(p.get()));
}

TEST_CASE("collect marks a single rooted object", "[collect]") {
  dustman::Root<Leaf> r {dustman::alloc<Leaf>()};
  dustman::collect();
  REQUIRE(dustman::detail::is_marked(r.get()));
}

TEST_CASE("collect does not mark unrooted objects", "[collect]") {
  NoEvacGuard g;
  dustman::Root<Leaf> r {dustman::alloc<Leaf>()};
  dustman::gc_ptr<Leaf> orphan = dustman::alloc<Leaf>();

  dustman::collect();

  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE_FALSE(dustman::detail::is_marked(orphan.get()));
}

TEST_CASE("collect follows tracer into gc_ptr fields", "[collect]") {
  dustman::Root<Node> r {dustman::alloc<Node>()};
  r->a = dustman::alloc<Leaf>();
  r->b = dustman::alloc<Leaf>();

  dustman::collect();

  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE(dustman::detail::is_marked(r->a.get()));
  REQUIRE(dustman::detail::is_marked(r->b.get()));
}

TEST_CASE("collect terminates on cyclic object graphs", "[collect]") {
  NoEvacGuard g;
  dustman::Root<Cyclic> r {dustman::alloc<Cyclic>()};
  auto other = dustman::alloc<Cyclic>();
  r->next = other;
  other->next = r;

  dustman::collect();

  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE(dustman::detail::is_marked(other.get()));
}

TEST_CASE("collect handles multiple roots", "[collect]") {
  dustman::Root<Leaf> r1 {dustman::alloc<Leaf>()};
  dustman::Root<Leaf> r2 {dustman::alloc<Leaf>()};
  dustman::Root<Leaf> r3 {dustman::alloc<Leaf>()};

  dustman::collect();

  REQUIRE(dustman::detail::is_marked(r1.get()));
  REQUIRE(dustman::detail::is_marked(r2.get()));
  REQUIRE(dustman::detail::is_marked(r3.get()));
}

TEST_CASE("clear_all_marks wipes every block's mark bitmap", "[collect]") {
  dustman::Root<Leaf> r {dustman::alloc<Leaf>()};
  dustman::collect();
  REQUIRE(dustman::detail::is_marked(r.get()));

  dustman::detail::clear_all_marks();
  REQUIRE_FALSE(dustman::detail::is_marked(r.get()));
}

TEST_CASE("collect un-marks objects that became unreachable between cycles", "[collect]") {
  NoEvacGuard g;
  dustman::Root<Leaf> r {dustman::alloc<Leaf>()};
  void* addr = nullptr;
  {
    dustman::Root<Leaf> orphan {dustman::alloc<Leaf>()};
    addr = orphan.get();
    dustman::collect();
    REQUIRE(dustman::detail::is_marked(addr));
  }

  dustman::collect();
  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE_FALSE(dustman::detail::is_marked(addr));
}

TEST_CASE("buggy tracer: missed field's pointee is left unmarked", "[collect]") {
  NoEvacGuard g;
  dustman::Root<BadlyTraced> r {dustman::alloc<BadlyTraced>()};
  r->traced = dustman::alloc<Leaf>();
  r->untraced = dustman::alloc<Leaf>();

  dustman::collect();

  REQUIRE(dustman::detail::is_marked(r.get()));
  REQUIRE(dustman::detail::is_marked(r->traced.get()));
  REQUIRE_FALSE(dustman::detail::is_marked(r->untraced.get()));
}

TEST_CASE("collect leaves gc_state idle on return", "[collect]") {
  REQUIRE(dustman::detail::gc_state == dustman::detail::GcState::idle);
  dustman::collect();
  REQUIRE(dustman::detail::gc_state == dustman::detail::GcState::idle);
}
