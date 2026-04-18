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

int sweep_count = 0;

struct SweepCounted {
  int tag = 0;
  explicit SweepCounted(int t) noexcept : tag(t) {}
  ~SweepCounted() noexcept { ++sweep_count; }
};

struct Survivor {
  int v = 0;
  explicit Survivor(int x) noexcept : v(x) {}
};

} // namespace

template <>
struct dustman::Tracer<SweepCounted> : dustman::FieldList<SweepCounted> {};

template <>
struct dustman::Tracer<Survivor> : dustman::FieldList<Survivor> {};

TEST_CASE("sweep destroys unreachable objects in all-dead blocks", "[sweep]") {
  const int before = sweep_count;

  constexpr std::size_t N = 4096;
  for (std::size_t i = 0; i < N; ++i) {
    (void)dustman::alloc<SweepCounted>(static_cast<int>(i));
  }

  dustman::collect();

  const int delta = sweep_count - before;
  REQUIRE(delta == static_cast<int>(N));
}

TEST_CASE("sweep retains rooted objects", "[sweep]") {
  dustman::Root<Survivor> r {dustman::alloc<Survivor>(42)};
  REQUIRE(r->v == 42);

  dustman::collect();

  REQUIRE(r->v == 42);
  REQUIRE(dustman::detail::is_marked(r.get()));
}

TEST_CASE("collect reduces the live block count when allocations are unreachable", "[sweep]") {
  constexpr std::size_t N = 4096;
  for (std::size_t i = 0; i < N; ++i) {
    (void)dustman::alloc<SweepCounted>(static_cast<int>(i));
  }

  const std::size_t before = dustman::detail::heap_block_count();
  dustman::collect();
  const std::size_t after = dustman::detail::heap_block_count();

  REQUIRE(after < before);
}

TEST_CASE("allocation works after collect", "[sweep]") {
  dustman::collect();

  auto p = dustman::alloc<Survivor>(7);
  REQUIRE(p.get() != nullptr);
  REQUIRE(p->v == 7);
}

TEST_CASE("a rooted object survives many alloc-and-collect cycles", "[sweep]") {
  dustman::Root<Survivor> r {dustman::alloc<Survivor>(99)};

  for (int cycle = 0; cycle < 4; ++cycle) {
    for (int i = 0; i < 500; ++i) {
      (void)dustman::alloc<SweepCounted>(i);
    }
    dustman::collect();
    REQUIRE(r->v == 99);
    REQUIRE(dustman::detail::is_marked(r.get()));
  }
}
