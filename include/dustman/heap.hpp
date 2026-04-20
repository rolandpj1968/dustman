#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dustman/gc_ptr.hpp"

namespace dustman {

class Visitor;

namespace detail {
inline std::atomic<bool> pause_requested_ {false};
extern thread_local bool attached_;

void safepoint_slow() noexcept;
} // namespace detail

inline void safepoint() noexcept {
  if (detail::pause_requested_.load(std::memory_order_acquire)) {
    detail::safepoint_slow();
  }
}

void attach_thread() noexcept;
void detach_thread() noexcept;

namespace detail {
inline std::atomic<std::uint32_t> evacuation_threshold_percent_ {25};

inline void ensure_attached() noexcept {
  if (!attached_) attach_thread();
}
}

inline void set_evacuation_threshold_percent(std::uint32_t p) noexcept {
  detail::evacuation_threshold_percent_.store(p, std::memory_order_relaxed);
}

inline std::uint32_t get_evacuation_threshold_percent() noexcept {
  return detail::evacuation_threshold_percent_.load(std::memory_order_relaxed);
}

namespace detail {

inline constexpr std::size_t block_size = 32 * 1024;
inline constexpr std::size_t block_alignment = block_size;

inline constexpr std::size_t slot_bytes = alignof(void*);
inline constexpr std::size_t max_slots_per_block = block_size / slot_bytes;
inline constexpr std::size_t bitmap_bytes = (max_slots_per_block + 7) / 8;

inline constexpr std::size_t line_size = 256;
inline constexpr std::size_t line_body_size = line_size - sizeof(void*);
inline constexpr std::size_t line_map_bytes = block_size / line_size;

inline constexpr std::size_t medium_size_limit = 4 * 1024;
inline constexpr std::size_t max_alignment = 4096;

inline constexpr std::uint32_t flag_block_medium = 1u << 0;
inline constexpr std::uint32_t flag_block_evacuating = 1u << 1;

enum class GcState : std::uint8_t {
  idle,
  marking,
  sweeping,
};

inline GcState gc_state = GcState::idle;

struct alignas(128) BlockHeader {
  std::uint32_t flags = 0;
  std::uint32_t live_count = 0;
  std::array<std::uint8_t, bitmap_bytes> mark_bitmap {};
  std::array<std::uint8_t, bitmap_bytes> start_bitmap {};
  std::array<std::uint8_t, line_map_bytes> line_map {};
};

inline constexpr std::size_t block_header_size = sizeof(BlockHeader);
inline constexpr std::size_t block_body_size = block_size - block_header_size;
inline constexpr std::size_t lines_per_block = block_body_size / line_size;

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

inline std::size_t line_of(const void* p) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  auto base = addr & ~(block_alignment - 1);
  auto body = base + block_header_size;
  return (addr - body) / line_size;
}

inline bool is_medium_block(const BlockHeader* h) noexcept {
  return (h->flags & flag_block_medium) != 0;
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

inline void clear_mark(const void* obj) noexcept {
  BlockHeader* h = header_of(obj);
  std::size_t slot = slot_index(obj);
  std::uint8_t mask = std::uint8_t(1) << (slot % 8);
  h->mark_bitmap[slot / 8] &= static_cast<std::uint8_t>(~mask);
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

extern thread_local Tlab small_tlab;
extern thread_local Tlab medium_tlab;

extern thread_local bool collecting_;

inline void* tlab_bump(Tlab& tlab, std::size_t size) noexcept {
  std::byte* cursor = tlab.cursor;
  std::byte* end = tlab.end;
  if (cursor != nullptr && static_cast<std::size_t>(end - cursor) >= size) {
    tlab.cursor = cursor + size;
    return cursor;
  }
  return nullptr;
}

void* alloc_slow_small(std::size_t size);
void* alloc_slow_medium(std::size_t size);
void* alloc_huge(std::size_t obj_bytes, std::size_t align);

bool mark_huge(const void* body) noexcept;
bool update_huge(const void* body) noexcept;
void sweep_huge() noexcept;
std::size_t huge_count() noexcept;

std::vector<BlockHeader*> classify_and_destroy_dead(std::uint32_t threshold_percent) noexcept;
void evacuate_block(BlockHeader* h);
void finalize_sweep() noexcept;

void clear_all_marks() noexcept;
std::size_t heap_block_count() noexcept;

bool acquire_collector_slot() noexcept;
void release_collector_slot() noexcept;

[[noreturn]] void fatal_oom() noexcept;
[[noreturn]] void fatal_reentrant_collect() noexcept;

std::size_t register_root_slot(gc_ptr_base* p) noexcept;
void unregister_root_slot(std::size_t slot) noexcept;
void update_root_slot(std::size_t slot, gc_ptr_base* p) noexcept;
void visit_roots(Visitor& v) noexcept;

} // namespace detail
} // namespace dustman
