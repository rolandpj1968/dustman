#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "dustman/gc_ptr.hpp"

namespace dustman {

class Visitor;

inline void safepoint() noexcept {}
inline void attach_thread() noexcept {}
inline void detach_thread() noexcept {}

namespace detail {

inline constexpr std::size_t block_size = 32 * 1024;
inline constexpr std::size_t block_alignment = block_size;

inline constexpr std::size_t slot_bytes = alignof(void*);
inline constexpr std::size_t max_slots_per_block = block_size / slot_bytes;
inline constexpr std::size_t bitmap_bytes = (max_slots_per_block + 7) / 8;

enum class GcState : std::uint8_t {
  idle,
  marking,
  sweeping,
};

inline GcState gc_state = GcState::idle;

struct alignas(64) BlockHeader {
  std::uint32_t flags = 0;
  std::uint32_t live_count = 0;
  std::array<std::uint8_t, bitmap_bytes> mark_bitmap {};
  std::array<std::uint8_t, bitmap_bytes> start_bitmap {};
};

inline constexpr std::size_t block_header_size = sizeof(BlockHeader);
inline constexpr std::size_t block_body_size = block_size - block_header_size;

inline BlockHeader* header_of(const void* p) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  auto base = addr & ~(block_alignment - 1);
  return reinterpret_cast<BlockHeader*>(base);
}

inline std::size_t slot_index(const void* p) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  auto base = addr & ~(block_alignment - 1);
  auto body = base + block_header_size;
  return (addr - body) / slot_bytes;
}

inline bool is_marked(const void* obj) noexcept {
  BlockHeader* h = header_of(obj);
  std::size_t slot = slot_index(obj);
  std::uint8_t mask = std::uint8_t(1) << (slot % 8);
  return (h->mark_bitmap[slot / 8] & mask) != 0;
}

inline void set_mark(const void* obj) noexcept {
  BlockHeader* h = header_of(obj);
  std::size_t slot = slot_index(obj);
  std::uint8_t mask = std::uint8_t(1) << (slot % 8);
  h->mark_bitmap[slot / 8] |= mask;
}

inline bool is_start(const void* obj) noexcept {
  BlockHeader* h = header_of(obj);
  std::size_t slot = slot_index(obj);
  std::uint8_t mask = std::uint8_t(1) << (slot % 8);
  return (h->start_bitmap[slot / 8] & mask) != 0;
}

inline void set_start(const void* obj) noexcept {
  BlockHeader* h = header_of(obj);
  std::size_t slot = slot_index(obj);
  std::uint8_t mask = std::uint8_t(1) << (slot % 8);
  h->start_bitmap[slot / 8] |= mask;
}

struct Tlab {
  std::byte* cursor = nullptr;
  std::byte* end = nullptr;
};

extern thread_local Tlab current_tlab;

extern thread_local bool collecting_;

void* alloc_slow(std::size_t size);

void clear_all_marks() noexcept;
void sweep_all_blocks() noexcept;
std::size_t heap_block_count() noexcept;

[[noreturn]] void fatal_oom() noexcept;
[[noreturn]] void fatal_reentrant_collect() noexcept;

std::size_t register_root_slot(gc_ptr_base* p) noexcept;
void unregister_root_slot(std::size_t slot) noexcept;
void update_root_slot(std::size_t slot, gc_ptr_base* p) noexcept;
void visit_roots(Visitor& v) noexcept;

} // namespace detail
} // namespace dustman
