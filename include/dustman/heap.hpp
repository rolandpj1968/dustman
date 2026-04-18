#pragma once

#include <cstddef>

#include "dustman/gc_ptr.hpp"

namespace dustman {

class Visitor;

namespace detail {

inline constexpr std::size_t block_size = 32 * 1024;
inline constexpr std::size_t block_alignment = block_size;

struct Tlab {
  std::byte* cursor = nullptr;
  std::byte* end = nullptr;
};

extern thread_local Tlab current_tlab;

void* alloc_slow(std::size_t size);

[[noreturn]] void fatal_oom() noexcept;

std::size_t register_root_slot(gc_ptr_base* p) noexcept;
void unregister_root_slot(std::size_t slot) noexcept;
void update_root_slot(std::size_t slot, gc_ptr_base* p) noexcept;
void visit_roots(Visitor& v) noexcept;

} // namespace detail
} // namespace dustman
