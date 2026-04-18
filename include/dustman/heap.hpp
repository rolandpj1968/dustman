#pragma once

#include <cstddef>

namespace dustman::detail {

inline constexpr std::size_t block_size = 32 * 1024;
inline constexpr std::size_t block_alignment = block_size;

struct Tlab {
  std::byte* cursor = nullptr;
  std::byte* end = nullptr;
};

extern thread_local Tlab current_tlab;

void* alloc_slow(std::size_t size);

[[noreturn]] void fatal_oom() noexcept;

} // namespace dustman::detail
