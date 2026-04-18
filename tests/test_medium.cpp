#include <array>
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

int medium_destroyed = 0;

struct MediumObj {
  std::array<int, 128> data;
  int tag = 0;
  explicit MediumObj(int t) noexcept : tag(t) { data.fill(t); }
  ~MediumObj() noexcept { ++medium_destroyed; }
};

struct Small {
  int a = 0;
  int b = 0;
};

} // namespace

template <>
struct dustman::Tracer<MediumObj> : dustman::FieldList<MediumObj> {};

template <>
struct dustman::Tracer<Small> : dustman::FieldList<Small> {};

TEST_CASE("medium allocation returns a usable object in a medium block", "[medium]") {
  auto p = dustman::alloc<MediumObj>(7);
  REQUIRE(p.get() != nullptr);
  REQUIRE(p->tag == 7);
  REQUIRE(p->data[0] == 7);
  REQUIRE(p->data[127] == 7);

  auto* h = dustman::detail::header_of(p.get());
  REQUIRE(dustman::detail::is_medium_block(h));
}

TEST_CASE("consecutive medium allocations return distinct addresses", "[medium]") {
  auto a = dustman::alloc<MediumObj>(1);
  auto b = dustman::alloc<MediumObj>(2);
  auto c = dustman::alloc<MediumObj>(3);

  REQUIRE(a.get() != b.get());
  REQUIRE(b.get() != c.get());
  REQUIRE(a->tag == 1);
  REQUIRE(b->tag == 2);
  REQUIRE(c->tag == 3);
}

TEST_CASE("rooted medium object survives collect", "[medium]") {
  dustman::Root<MediumObj> r {dustman::alloc<MediumObj>(42)};
  REQUIRE(r->tag == 42);

  dustman::collect();

  REQUIRE(r->tag == 42);
  REQUIRE(dustman::detail::is_marked(r.get()));
}

TEST_CASE("unreachable medium objects are destroyed by sweep", "[medium]") {
  const int before = medium_destroyed;

  constexpr int N = 200;
  for (int i = 0; i < N; ++i) {
    (void)dustman::alloc<MediumObj>(i);
  }

  dustman::collect();

  REQUIRE(medium_destroyed - before == N);
}

TEST_CASE("small and medium tiers use distinct block pools", "[medium]") {
  auto s = dustman::alloc<Small>();
  auto m = dustman::alloc<MediumObj>(1);

  auto* sh = dustman::detail::header_of(s.get());
  auto* mh = dustman::detail::header_of(m.get());

  REQUIRE(sh != mh);
  REQUIRE_FALSE(dustman::detail::is_medium_block(sh));
  REQUIRE(dustman::detail::is_medium_block(mh));
}

TEST_CASE("mixed small + medium workload: rooted objects survive collect", "[medium]") {
  dustman::Root<Small> small_root {dustman::alloc<Small>()};
  dustman::Root<MediumObj> medium_root {dustman::alloc<MediumObj>(99)};

  for (int i = 0; i < 50; ++i) {
    (void)dustman::alloc<Small>();
    (void)dustman::alloc<MediumObj>(i);
  }

  dustman::collect();

  REQUIRE(small_root.get() != nullptr);
  REQUIRE(medium_root->tag == 99);
  REQUIRE(dustman::detail::is_marked(small_root.get()));
  REQUIRE(dustman::detail::is_marked(medium_root.get()));
}
