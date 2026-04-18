#pragma once

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
  static_assert(alignof(T) <= alignof(void*),
                "dustman phase 1: over-aligned types are not yet supported");
  constexpr std::size_t hdr = sizeof(const TypeInfo*);
  constexpr std::size_t total = hdr + sizeof(T);
  return round_up(total, alignof(void*));
}

template <typename T>
constexpr std::uint32_t compute_type_flags() noexcept {
  return (object_bytes<T>() > medium_size_limit) ? flag_huge : 0;
}

} // namespace detail

template <typename T>
struct TypeInfoFor {
  static_assert(std::is_nothrow_destructible_v<T>,
                "dustman: GC-managed types must be nothrow-destructible");

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
  return *hdr;
}

} // namespace dustman
