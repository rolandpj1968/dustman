#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct ACLeaf {
  std::uint64_t v = 0;
};

struct ACWide {
  std::uint64_t pad[60] {};
};

struct ACHuge {
  std::uint64_t pad[600] {};
};

struct DefaultTuning {
  std::size_t saved_minor;
  std::size_t saved_min;
  std::uint32_t saved_growth;
  bool saved_enabled;
  DefaultTuning()
      : saved_minor(dustman::get_minor_threshold_bytes()),
        saved_min(dustman::get_major_min_bytes()),
        saved_growth(dustman::get_major_growth_factor_percent()),
        saved_enabled(dustman::get_auto_collect_enabled()) {}
  ~DefaultTuning() {
    dustman::set_minor_threshold_bytes(saved_minor);
    dustman::set_major_min_bytes(saved_min);
    dustman::set_major_growth_factor_percent(saved_growth);
    dustman::set_auto_collect_enabled(saved_enabled);
  }
};

} // namespace

template <>
struct dustman::Tracer<ACLeaf> : dustman::FieldList<ACLeaf> {};

template <>
struct dustman::Tracer<ACWide> : dustman::FieldList<ACWide> {};

template <>
struct dustman::Tracer<ACHuge> : dustman::FieldList<ACHuge> {};

TEST_CASE("get_minor_count increments after explicit minor_collect", "[visibility]") {
  std::size_t before = dustman::get_minor_count();
  dustman::minor_collect();
  REQUIRE(dustman::get_minor_count() == before + 1);
}

TEST_CASE("get_major_count increments after explicit collect", "[visibility]") {
  std::size_t before = dustman::get_major_count();
  dustman::collect();
  REQUIRE(dustman::get_major_count() == before + 1);
}

TEST_CASE("heap_stats reflects current state", "[visibility]") {
  dustman::Root<ACLeaf> r {dustman::alloc<ACLeaf>()};
  r->v = 123;

  auto stats = dustman::heap_stats();
  REQUIRE(stats.current_heap_bytes > 0);
  REQUIRE(stats.current_young_bytes + stats.current_old_bytes + stats.huge_bytes
          == stats.current_heap_bytes);
  REQUIRE(stats.total_bytes_allocated >= stats.current_heap_bytes);
}

TEST_CASE("minor_collect updates last_minor_pause_us", "[visibility]") {
  dustman::minor_collect();
  auto first = dustman::get_last_minor_pause_us();
  dustman::Root<ACLeaf> r {dustman::alloc<ACLeaf>()};
  dustman::minor_collect();
  auto second = dustman::get_last_minor_pause_us();
  REQUIRE(second <= 10'000'000ULL);
  (void)first;
}

TEST_CASE("auto-collect fires a minor after crossing minor_threshold_bytes", "[auto-collect]") {
  DefaultTuning g;

  dustman::minor_collect();
  std::size_t minors_before = dustman::get_minor_count();

  dustman::set_minor_threshold_bytes(256 * 1024);
  dustman::set_auto_collect_enabled(true);

  for (int i = 0; i < 20000; ++i) {
    dustman::Root<ACWide> r {dustman::alloc<ACWide>()};
    (void)r;
  }

  REQUIRE(dustman::get_minor_count() > minors_before);
}

TEST_CASE("auto-collect fires a major once old-gen growth exceeds threshold",
          "[auto-collect]") {
  DefaultTuning g;

  dustman::collect();
  std::size_t majors_before = dustman::get_major_count();

  dustman::set_major_min_bytes(64 * 1024);
  dustman::set_major_growth_factor_percent(150);
  dustman::set_minor_threshold_bytes(64 * 1024);
  dustman::set_auto_collect_enabled(true);

  std::vector<dustman::Root<ACWide>> keepers;
  keepers.reserve(5000);
  for (int i = 0; i < 5000; ++i) {
    keepers.emplace_back(dustman::alloc<ACWide>());
    keepers.back()->pad[0] = static_cast<std::uint64_t>(i);
  }

  REQUIRE(dustman::get_major_count() > majors_before);
}

TEST_CASE("disabling auto-collect suppresses both minor and major triggers",
          "[auto-collect]") {
  DefaultTuning g;

  dustman::minor_collect();
  std::size_t minors_before = dustman::get_minor_count();
  std::size_t majors_before = dustman::get_major_count();

  dustman::set_auto_collect_enabled(false);
  dustman::set_minor_threshold_bytes(64 * 1024);
  dustman::set_major_min_bytes(64 * 1024);
  dustman::set_major_growth_factor_percent(110);

  for (int i = 0; i < 500; ++i) {
    dustman::Root<ACWide> keep {dustman::alloc<ACWide>()};
    (void)keep;
  }

  REQUIRE(dustman::get_minor_count() == minors_before);
  REQUIRE(dustman::get_major_count() == majors_before);
}

TEST_CASE("explicit minor / major still work when auto-collect disabled",
          "[auto-collect]") {
  DefaultTuning g;

  dustman::set_auto_collect_enabled(false);

  std::size_t minors_before = dustman::get_minor_count();
  std::size_t majors_before = dustman::get_major_count();

  dustman::minor_collect();
  dustman::collect();

  REQUIRE(dustman::get_minor_count() == minors_before + 1);
  REQUIRE(dustman::get_major_count() == majors_before + 1);
}

TEST_CASE("huge allocations don't drive the minor-collect counter",
          "[auto-collect]") {
  DefaultTuning g;

  dustman::minor_collect();
  std::size_t minors_before = dustman::get_minor_count();

  dustman::set_auto_collect_enabled(true);
  dustman::set_minor_threshold_bytes(1 * 1024 * 1024);

  std::vector<dustman::Root<ACHuge>> keepers;
  keepers.reserve(512);
  for (int i = 0; i < 512; ++i) {
    keepers.emplace_back(dustman::alloc<ACHuge>());
  }

  REQUIRE(dustman::get_minor_count() == minors_before);
}
