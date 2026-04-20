#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/heap.hpp"
#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"

namespace {

struct Stress {
  std::uint32_t a;
  std::uint32_t b;
};

} // namespace

template <>
struct dustman::Tracer<Stress> : dustman::FieldList<Stress> {};

// This test measures raw allocator mechanics (TypeInfo header, alignment,
// bytes in the body).  It holds Stress* raw pointers across many allocations
// without rooting them, which is fine for the allocator's POV but would be
// reaped by the auto-collect policy.  Disable auto-collect for this test;
// restore on exit.
struct NoAutoCollectGuard {
  bool saved;
  NoAutoCollectGuard() : saved(dustman::get_auto_collect_enabled()) {
    dustman::set_auto_collect_enabled(false);
  }
  ~NoAutoCollectGuard() { dustman::set_auto_collect_enabled(saved); }
};

TEST_CASE("arena stress: 500k allocations preserve TypeInfo, alignment, and values",
          "[alloc][stress]") {
  NoAutoCollectGuard g;

  constexpr std::size_t N = 500'000;

  std::vector<Stress*> addrs;
  addrs.reserve(N);

  for (std::size_t i = 0; i < N; ++i) {
    auto p = dustman::alloc<Stress>();
    p->a = static_cast<std::uint32_t>(i);
    p->b = static_cast<std::uint32_t>(~i);
    addrs.push_back(p.get());
  }

  REQUIRE(addrs.size() == N);

  const dustman::TypeInfo* expected_ti = &dustman::TypeInfoFor<Stress>::value;

  std::size_t bad_type = 0;
  std::size_t bad_align = 0;
  std::size_t bad_value = 0;
  for (std::size_t i = 0; i < N; ++i) {
    Stress* p = addrs[i];

    if (dustman::type_of(p) != expected_ti) {
      ++bad_type;
    }
    if (reinterpret_cast<std::uintptr_t>(p) & (alignof(void*) - 1)) {
      ++bad_align;
    }
    if (p->a != static_cast<std::uint32_t>(i) || p->b != static_cast<std::uint32_t>(~i)) {
      ++bad_value;
    }
  }

  REQUIRE(bad_type == 0);
  REQUIRE(bad_align == 0);
  REQUIRE(bad_value == 0);

  std::vector<Stress*> sorted = addrs;
  std::sort(sorted.begin(), sorted.end());

  std::size_t bad_overlap = 0;
  for (std::size_t i = 1; i < N; ++i) {
    auto prev = reinterpret_cast<std::uintptr_t>(sorted[i - 1]);
    auto cur = reinterpret_cast<std::uintptr_t>(sorted[i]);
    if (cur < prev + sizeof(Stress)) {
      ++bad_overlap;
    }
  }
  REQUIRE(bad_overlap == 0);
}
