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

template <typename T>
inline void* alloc_raw() {
  constexpr std::size_t size = object_bytes<T>();
  static_assert(size <= block_size, "dustman: allocation too large for a single block");

  Tlab& tlab = current_tlab;
  std::byte* cursor = tlab.cursor;
  std::byte* end = tlab.end;
  if (cursor != nullptr) {
    const std::size_t available = static_cast<std::size_t>(end - cursor);
    if (available >= size) {
      tlab.cursor = cursor + size;
      return cursor;
    }
  }
  return alloc_slow(size);
}

} // namespace detail

template <typename T, typename... Args>
gc_ptr<T> alloc(Args&&... args) {
  void* hdr = detail::alloc_raw<T>();
  *static_cast<const TypeInfo**>(hdr) = &TypeInfoFor<T>::value;
  void* body = static_cast<std::byte*>(hdr) + sizeof(const TypeInfo*);
  T* obj = new (body) T(std::forward<Args>(args)...);
  return gc_ptr<T> {obj};
}

template <typename T>
const TypeInfo* type_of(const T* obj) noexcept {
  auto* bytes = reinterpret_cast<const std::byte*>(obj);
  auto* hdr = reinterpret_cast<const TypeInfo* const*>(bytes - sizeof(const TypeInfo*));
  return *hdr;
}

} // namespace dustman
