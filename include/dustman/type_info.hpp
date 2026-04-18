#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "dustman/tracer.hpp"

namespace dustman {

class Visitor;

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

template<typename T>
void trace_trampoline(void* obj, Visitor& v) {
  Tracer<T>::trace(*static_cast<T*>(obj), v);
}

template<typename T>
void destroy_trampoline(void* obj) noexcept {
  static_cast<T*>(obj)->~T();
}

}  // namespace detail

template<typename T>
struct TypeInfoFor {
  static_assert(std::is_nothrow_destructible_v<T>,
                "dustman: GC-managed types must be nothrow-destructible");

  static constexpr TypeInfo value{
      sizeof(T),
      alignof(T),
      &detail::trace_trampoline<T>,
      &detail::destroy_trampoline<T>,
      0,
  };
};

}  // namespace dustman
