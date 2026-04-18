#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct Tiny {
  int v = 0;
};

} // namespace

template <>
struct dustman::Tracer<Tiny> : dustman::FieldList<Tiny> {};

TEST_CASE("recycle list reuses small blocks after collect", "[recycle]") {
  dustman::Root<Tiny> pinned {dustman::alloc<Tiny>()};

  constexpr std::size_t N = 200;
  for (std::size_t i = 0; i < N; ++i) {
    (void)dustman::alloc<Tiny>();
  }

  const std::size_t before = dustman::detail::heap_block_count();

  dustman::collect();

  const std::size_t after_collect = dustman::detail::heap_block_count();

  for (std::size_t i = 0; i < N; ++i) {
    (void)dustman::alloc<Tiny>();
  }

  const std::size_t after_realloc = dustman::detail::heap_block_count();

  REQUIRE(after_collect <= before);
  REQUIRE(after_realloc <= after_collect + 1);
  REQUIRE(pinned.get() != nullptr);
}
