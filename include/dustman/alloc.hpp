#pragma once

#include <cstddef>
#include <new>
#include <utility>

#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/type_info.hpp"

namespace dustman {

namespace detail {

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

template <typename T, typename... Args>
inline gc_ptr<T> alloc_construct(void* hdr, Args&&... args) {
  *static_cast<const TypeInfo**>(hdr) = &TypeInfoFor<T>::value;
  void* body = static_cast<std::byte*>(hdr) + sizeof(const TypeInfo*);
  set_start(body);
  T* obj = new (body) T(std::forward<Args>(args)...);
  return gc_ptr<T> {obj};
}

} // namespace detail

template <typename T, typename... Args>
gc_ptr<T> alloc(Args&&... args) {
  if (detail::collecting_) {
    detail::fatal_reentrant_collect();
  }

  constexpr std::size_t size = detail::object_bytes<T>();
  static_assert(size <= detail::medium_size_limit,
                "dustman phase 3a-iv: huge tier (> 4 KiB) not yet implemented");

  void* hdr;
  if constexpr (size <= detail::line_size) {
    hdr = detail::tlab_bump(detail::small_tlab, size);
    if (hdr == nullptr)
      hdr = detail::alloc_slow_small(size);
  } else {
    hdr = detail::tlab_bump(detail::medium_tlab, size);
    if (hdr == nullptr)
      hdr = detail::alloc_slow_medium(size);
  }

  return detail::alloc_construct<T>(hdr, std::forward<Args>(args)...);
}

} // namespace dustman
