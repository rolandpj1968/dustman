#pragma once

#include <cstddef>

#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"

namespace dustman {

template <typename T>
class Root {
public:
  Root() noexcept : slot_(detail::register_root_slot(&payload_)) {}
  Root(std::nullptr_t) noexcept : Root() {}
  Root(gc_ptr<T> p) noexcept : payload_(p), slot_(detail::register_root_slot(&payload_)) {}

  ~Root() noexcept {
    if (slot_ != invalid_slot) {
      detail::unregister_root_slot(slot_);
    }
  }

  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  Root(Root&& other) noexcept : payload_(other.payload_), slot_(other.slot_) {
    detail::update_root_slot(slot_, &payload_);
    other.slot_ = invalid_slot;
  }

  Root& operator=(Root&& other) noexcept {
    if (this != &other) {
      if (slot_ != invalid_slot) {
        detail::unregister_root_slot(slot_);
      }
      payload_ = other.payload_;
      slot_ = other.slot_;
      detail::update_root_slot(slot_, &payload_);
      other.slot_ = invalid_slot;
    }
    return *this;
  }

  Root& operator=(gc_ptr<T> p) noexcept {
    payload_ = p;
    return *this;
  }

  Root& operator=(std::nullptr_t) noexcept {
    payload_ = nullptr;
    return *this;
  }

  T* get() const noexcept { return payload_.get(); }
  T& operator*() const noexcept { return *payload_; }
  T* operator->() const noexcept { return payload_.operator->(); }

  explicit operator bool() const noexcept { return static_cast<bool>(payload_); }

  operator gc_ptr<T>() const noexcept { return payload_; }

private:
  gc_ptr<T> payload_;
  std::size_t slot_;

  static constexpr std::size_t invalid_slot = static_cast<std::size_t>(-1);
};

} // namespace dustman
