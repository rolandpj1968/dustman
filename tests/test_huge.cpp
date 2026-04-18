#include <array>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"

namespace {

int huge_destroyed = 0;

struct HugeObj {
  std::array<int, 2048> data;
  int tag = 0;

  explicit HugeObj(int t) noexcept : tag(t) { data.fill(t); }
  ~HugeObj() noexcept { ++huge_destroyed; }
};

struct HugeWithChild {
  std::array<int, 2048> padding;
  dustman::gc_ptr<HugeObj> child;
};

} // namespace

template <>
struct dustman::Tracer<HugeObj> : dustman::FieldList<HugeObj> {};

template <>
struct dustman::Tracer<HugeWithChild> : dustman::FieldList<HugeWithChild, &HugeWithChild::child> {};

TEST_CASE("object_bytes<HugeObj> exceeds the medium size limit", "[huge]") {
  STATIC_REQUIRE(dustman::detail::object_bytes<HugeObj>() > dustman::detail::medium_size_limit);
}

TEST_CASE("flag_huge is set on TypeInfo for huge types", "[huge]") {
  STATIC_REQUIRE((dustman::TypeInfoFor<HugeObj>::value.flags & dustman::flag_huge) != 0);
}

TEST_CASE("huge allocation returns a usable object with correct contents", "[huge]") {
  auto p = dustman::alloc<HugeObj>(7);
  REQUIRE(p.get() != nullptr);
  REQUIRE(p->tag == 7);
  REQUIRE(p->data[0] == 7);
  REQUIRE(p->data[2047] == 7);
}

TEST_CASE("consecutive huge allocations return distinct addresses", "[huge]") {
  auto a = dustman::alloc<HugeObj>(1);
  auto b = dustman::alloc<HugeObj>(2);
  auto c = dustman::alloc<HugeObj>(3);

  REQUIRE(a.get() != b.get());
  REQUIRE(b.get() != c.get());
  REQUIRE(a->tag == 1);
  REQUIRE(b->tag == 2);
  REQUIRE(c->tag == 3);
}

TEST_CASE("rooted huge object survives collect", "[huge]") {
  dustman::Root<HugeObj> r {dustman::alloc<HugeObj>(42)};
  REQUIRE(r->tag == 42);

  dustman::collect();

  REQUIRE(r->tag == 42);
}

TEST_CASE("unreachable huge objects are destroyed by sweep", "[huge]") {
  const int before = huge_destroyed;

  constexpr int N = 8;
  for (int i = 0; i < N; ++i) {
    (void)dustman::alloc<HugeObj>(i);
  }

  dustman::collect();

  REQUIRE(huge_destroyed - before == N);
}

TEST_CASE("collect traces gc_ptr<HugeObj> fields from a rooted huge parent", "[huge]") {
  dustman::Root<HugeWithChild> parent {dustman::alloc<HugeWithChild>()};
  parent->child = dustman::alloc<HugeObj>(123);
  auto* child_addr = parent->child.get();

  const int before = huge_destroyed;
  dustman::collect();
  const int delta = huge_destroyed - before;

  REQUIRE(parent->child.get() == child_addr);
  REQUIRE(parent->child->tag == 123);
  REQUIRE(delta == 0);
}

TEST_CASE("huge_count reflects live huge objects after a collect cycle", "[huge]") {
  {
    dustman::Root<HugeObj> r {dustman::alloc<HugeObj>(1)};
    (void)dustman::alloc<HugeObj>(2);
    (void)dustman::alloc<HugeObj>(3);
    dustman::collect();
    REQUIRE(dustman::detail::huge_count() >= 1);
  }
  dustman::collect();
  REQUIRE(dustman::detail::huge_count() == 0);
}
