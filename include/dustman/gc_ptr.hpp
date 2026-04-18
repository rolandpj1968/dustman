#pragma once

#include <cstddef>

namespace dustman {

class gc_ptr_base {
public:
  gc_ptr_base() noexcept = default;
  explicit gc_ptr_base(void* p) noexcept : raw_(p) {}

  void* load() const noexcept { return raw_; }
  void store(void* p) noexcept { raw_ = p; }

protected:
  void* raw_ = nullptr;
};

template<typename T>
class gc_ptr : public gc_ptr_base {
public:
  gc_ptr() noexcept = default;
  gc_ptr(std::nullptr_t) noexcept {}
  explicit gc_ptr(T* p) noexcept : gc_ptr_base(p) {}

  T* get() const noexcept { return static_cast<T*>(load()); }
  T& operator*() const noexcept { return *get(); }
  T* operator->() const noexcept { return get(); }

  explicit operator bool() const noexcept { return load() != nullptr; }

  gc_ptr& operator=(std::nullptr_t) noexcept {
    store(nullptr);
    return *this;
  }

  friend bool operator==(const gc_ptr& a, const gc_ptr& b) noexcept {
    return a.load() == b.load();
  }
  friend bool operator!=(const gc_ptr& a, const gc_ptr& b) noexcept {
    return a.load() != b.load();
  }
  friend bool operator==(const gc_ptr& a, std::nullptr_t) noexcept {
    return a.load() == nullptr;
  }
  friend bool operator!=(const gc_ptr& a, std::nullptr_t) noexcept {
    return a.load() != nullptr;
  }
  friend bool operator==(std::nullptr_t, const gc_ptr& a) noexcept {
    return a.load() == nullptr;
  }
  friend bool operator!=(std::nullptr_t, const gc_ptr& a) noexcept {
    return a.load() != nullptr;
  }
};

}  // namespace dustman
