#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"

namespace {

struct Small {
  int a;
  int b;
};

struct WithCtorArgs {
  int x;
  int y;
  WithCtorArgs(int ax, int ay) noexcept : x(ax), y(ay) {}
};

int destroyed_count = 0;

struct Destructible {
  int tag;
  explicit Destructible(int t) noexcept : tag(t) {}
  ~Destructible() noexcept { ++destroyed_count; }
};

} // namespace

template <>
struct dustman::Tracer<Small> : dustman::FieldList<Small> {};

template <>
struct dustman::Tracer<WithCtorArgs> : dustman::FieldList<WithCtorArgs> {};

template <>
struct dustman::Tracer<Destructible> : dustman::FieldList<Destructible> {};

TEST_CASE("alloc returns a non-null gc_ptr", "[alloc]") {
  auto p = dustman::alloc<Small>();
  REQUIRE(p != nullptr);
  REQUIRE(p.get() != nullptr);
}

TEST_CASE("alloc returns distinct addresses for consecutive calls", "[alloc]") {
  auto a = dustman::alloc<Small>();
  auto b = dustman::alloc<Small>();
  auto c = dustman::alloc<Small>();
  REQUIRE(a.get() != b.get());
  REQUIRE(b.get() != c.get());
  REQUIRE(a.get() != c.get());
}

TEST_CASE("alloc forwards constructor arguments", "[alloc]") {
  auto p = dustman::alloc<WithCtorArgs>(3, 7);
  REQUIRE(p->x == 3);
  REQUIRE(p->y == 7);
}

TEST_CASE("allocated object carries its TypeInfo in the header", "[alloc]") {
  auto p = dustman::alloc<Small>();
  const dustman::TypeInfo* ti = dustman::type_of(p.get());
  REQUIRE(ti == &dustman::TypeInfoFor<Small>::value);
  REQUIRE(ti->size == sizeof(Small));
  REQUIRE(ti->align == alignof(Small));
}

TEST_CASE("TypeInfo.destroy invokes the allocated object's destructor", "[alloc]") {
  destroyed_count = 0;
  auto p = dustman::alloc<Destructible>(42);
  REQUIRE(p->tag == 42);

  const dustman::TypeInfo* ti = dustman::type_of(p.get());
  ti->destroy(p.get());

  REQUIRE(destroyed_count == 1);
}

TEST_CASE("allocations span multiple blocks under load", "[alloc]") {
  constexpr std::size_t allocations = 4096;
  std::vector<void*> seen;
  seen.reserve(allocations);

  for (std::size_t i = 0; i < allocations; ++i) {
    auto p = dustman::alloc<Small>();
    seen.push_back(p.get());
  }

  REQUIRE(seen.size() == allocations);

  std::uintptr_t first_block =
      reinterpret_cast<std::uintptr_t>(seen.front()) & ~(dustman::detail::block_alignment - 1);
  std::uintptr_t last_block =
      reinterpret_cast<std::uintptr_t>(seen.back()) & ~(dustman::detail::block_alignment - 1);
  REQUIRE(first_block != last_block);
}

TEST_CASE("allocation respects alignof(void*) for the returned body", "[alloc]") {
  auto p = dustman::alloc<Small>();
  auto addr = reinterpret_cast<std::uintptr_t>(p.get());
  REQUIRE((addr & (alignof(void*) - 1)) == 0);
}
