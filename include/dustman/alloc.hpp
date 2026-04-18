#pragma once

#include <cstddef>
#include <new>
#include <utility>

#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/type_info.hpp"

namespace dustman {

template <typename T, typename... Args>
gc_ptr<T> alloc(Args&&... args) {
  if (detail::collecting_) {
    detail::fatal_reentrant_collect();
  }

  constexpr std::size_t size = detail::object_bytes<T>();
  constexpr std::uint32_t flags = detail::compute_type_flags<T>();

  void* hdr;
  if constexpr ((flags & flag_huge) != 0) {
    hdr = detail::alloc_huge(size, alignof(T));
  } else if constexpr (size <= detail::line_size) {
    hdr = detail::tlab_bump(detail::small_tlab, size);
    if (hdr == nullptr)
      hdr = detail::alloc_slow_small(size);
  } else {
    hdr = detail::tlab_bump(detail::medium_tlab, size);
    if (hdr == nullptr)
      hdr = detail::alloc_slow_medium(size);
  }

  *static_cast<const TypeInfo**>(hdr) = &TypeInfoFor<T>::value;
  void* body = static_cast<std::byte*>(hdr) + sizeof(const TypeInfo*);
  if constexpr ((flags & flag_huge) == 0) {
    detail::set_start(body);
  }
  T* obj = new (body) T(std::forward<Args>(args)...);
  return gc_ptr<T> {obj};
}

} // namespace dustman
