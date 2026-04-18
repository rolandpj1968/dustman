#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/heap.hpp"
#include "dustman/tracer.hpp"

namespace {

struct Small {
  int a;
  int b;
};

} // namespace

template <>
struct dustman::Tracer<Small> : dustman::FieldList<Small> {};

TEST_CASE("BlockHeader is 128-aligned and sized for line-aligned body", "[block]") {
  STATIC_REQUIRE(alignof(dustman::detail::BlockHeader) == 128);
  STATIC_REQUIRE(sizeof(dustman::detail::BlockHeader) % 128 == 0);
  STATIC_REQUIRE(dustman::detail::block_header_size % dustman::detail::line_size == 0);
}

TEST_CASE("block geometry is self-consistent", "[block]") {
  STATIC_REQUIRE(dustman::detail::block_size == 32 * 1024);
  STATIC_REQUIRE(dustman::detail::block_alignment == dustman::detail::block_size);
  STATIC_REQUIRE(dustman::detail::slot_bytes == alignof(void*));
  STATIC_REQUIRE(dustman::detail::line_size == 256);
  STATIC_REQUIRE(dustman::detail::line_body_size == 248);
  STATIC_REQUIRE(dustman::detail::block_body_size ==
                 dustman::detail::block_size - dustman::detail::block_header_size);
  STATIC_REQUIRE(dustman::detail::bitmap_bytes * 8 * dustman::detail::slot_bytes >=
                 dustman::detail::block_size);
  STATIC_REQUIRE(dustman::detail::line_map_bytes >= dustman::detail::lines_per_block);
  STATIC_REQUIRE(dustman::detail::lines_per_block * dustman::detail::line_size <=
                 dustman::detail::block_body_size);
}

TEST_CASE("header_of() returns the 32 KiB-aligned base of the containing block", "[block]") {
  auto p = dustman::alloc<Small>();
  auto* h = dustman::detail::header_of(p.get());

  auto header_addr = reinterpret_cast<std::uintptr_t>(h);
  REQUIRE((header_addr & (dustman::detail::block_alignment - 1)) == 0);

  auto body_addr = reinterpret_cast<std::uintptr_t>(p.get());
  REQUIRE(body_addr > header_addr);
  REQUIRE(body_addr >= header_addr + dustman::detail::block_header_size);
  REQUIRE(body_addr < header_addr + dustman::detail::block_size);
}

TEST_CASE("block header has zero flags and live_count after allocation", "[block]") {
  auto p = dustman::alloc<Small>();
  auto* h = dustman::detail::header_of(p.get());

  REQUIRE(h->flags == 0);
  REQUIRE(h->live_count == 0);
}

TEST_CASE("slot_index returns monotonically increasing values within a block", "[block]") {
  constexpr std::size_t N = 128;
  std::vector<void*> ptrs;
  ptrs.reserve(N);
  for (std::size_t i = 0; i < N; ++i) {
    ptrs.push_back(dustman::alloc<Small>().get());
  }

  auto* first_block = dustman::detail::header_of(ptrs[0]);

  std::size_t prev_slot = dustman::detail::slot_index(ptrs[0]);
  std::size_t same_block_count = 1;
  for (std::size_t i = 1; i < N; ++i) {
    if (dustman::detail::header_of(ptrs[i]) == first_block) {
      std::size_t slot = dustman::detail::slot_index(ptrs[i]);
      REQUIRE(slot > prev_slot);
      prev_slot = slot;
      ++same_block_count;
    }
  }

  REQUIRE(same_block_count >= 2);
}

TEST_CASE("engine API stubs compile and do nothing", "[engine]") {
  dustman::safepoint();
  dustman::attach_thread();
  dustman::detach_thread();
  SUCCEED("engine stubs are callable");
}

TEST_CASE("collection state defaults to idle", "[engine]") {
  REQUIRE(dustman::detail::gc_state == dustman::detail::GcState::idle);
}
