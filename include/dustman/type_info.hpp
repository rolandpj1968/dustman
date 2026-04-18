#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "dustman/heap.hpp"
#include "dustman/tracer.hpp"

namespace dustman {

class Visitor;

inline constexpr std::uint32_t flag_huge = 1u << 0;

using TraceFn = void (*)(void*, Visitor&);
using DestroyFn = void (*)(void*) noexcept;

struct TypeInfo {
  std::size_t size;
  std::size_t align;
  TraceFn trace;
  DestroyFn destroy;
  std::uint32_t flags;
};

namespace detail {

inline constexpr std::uintptr_t forwarded_bit = 1;

inline bool is_forwarded(const void* body) noexcept {
  auto word = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::byte*>(body) -
                                                       sizeof(void*));
  return (word & forwarded_bit) != 0;
}

inline void* forwarded_to(const void* body) noexcept {
  auto word = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::byte*>(body) -
                                                       sizeof(void*));
  return reinterpret_cast<void*>(word & ~forwarded_bit);
}

inline void set_forwarded(void* old_body, void* new_body) noexcept {
  auto word = reinterpret_cast<std::uintptr_t>(new_body) | forwarded_bit;
  auto* slot =
      reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::byte*>(old_body) - sizeof(void*));
  *slot = word;
}

enum class HeaderKind : std::uint8_t {
  Normal,
  Forwarded,
};

struct HeaderView {
  HeaderKind kind;
  const TypeInfo* type;
  void* new_body;
};

inline HeaderView decode_header(const void* body) noexcept {
  auto word = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::byte*>(body) -
                                                       sizeof(void*));
  HeaderView v {};
  if ((word & forwarded_bit) != 0) {
    v.kind = HeaderKind::Forwarded;
    v.new_body = reinterpret_cast<void*>(word & ~forwarded_bit);
  } else {
    v.kind = HeaderKind::Normal;
    v.type = reinterpret_cast<const TypeInfo*>(word);
  }
  return v;
}

template <typename T>
void trace_trampoline(void* obj, Visitor& v) {
  Tracer<T>::trace(*static_cast<T*>(obj), v);
}

template <typename T>
void destroy_trampoline(void* obj) noexcept {
  auto* t = static_cast<T*>(obj);
  t->~T();
}

constexpr std::size_t round_up(std::size_t x, std::size_t align) noexcept {
  return (x + align - 1) & ~(align - 1);
}

template <typename T>
constexpr std::size_t object_bytes() noexcept {
  static_assert(alignof(T) <= max_alignment,
                "dustman: alignof(T) exceeds max_alignment (see docs/allocation.md)");
  constexpr std::size_t hdr = sizeof(const TypeInfo*);
  constexpr std::size_t total = hdr + sizeof(T);
  return round_up(total, alignof(void*));
}

template <typename T>
constexpr std::uint32_t compute_type_flags() noexcept {
  const bool is_big = object_bytes<T>() > medium_size_limit;
  const bool is_wide = alignof(T) > alignof(void*);
  return (is_big || is_wide) ? flag_huge : 0;
}

} // namespace detail

template <typename T>
struct TypeInfoFor {
  static_assert(std::is_nothrow_destructible_v<T>,
                "dustman: GC-managed types must be nothrow-destructible");

#if defined(__has_builtin)
#  if __has_builtin(__is_trivially_relocatable)
  static_assert(__is_trivially_relocatable(T),
                "dustman: GC-managed types must be trivially relocatable "
                "(see consumer contract in docs/collection.md)");
#  endif
#endif

  static constexpr TypeInfo value {
      sizeof(T),
      alignof(T),
      &detail::trace_trampoline<T>,
      &detail::destroy_trampoline<T>,
      detail::compute_type_flags<T>(),
  };
};

inline const TypeInfo* type_of(const void* obj) noexcept {
  auto* bytes = reinterpret_cast<const std::byte*>(obj);
  auto* hdr = reinterpret_cast<const TypeInfo* const*>(bytes - sizeof(const TypeInfo*));
  assert(!detail::is_forwarded(obj) && "type_of called on a forwarded object");
  return *hdr;
}

inline std::size_t object_bytes_of(const TypeInfo* ti) noexcept {
  std::size_t total = sizeof(const TypeInfo*) + ti->size;
  return (total + alignof(void*) - 1) & ~(alignof(void*) - 1);
}

} // namespace dustman
