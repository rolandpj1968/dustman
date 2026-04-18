#include "dustman/heap.hpp"

#include <cstdlib>
#include <mutex>
#include <vector>

#include "dustman/gc_ptr.hpp"
#include "dustman/visitor.hpp"

namespace dustman::detail {

thread_local Tlab current_tlab;

thread_local std::vector<gc_ptr_base*> root_slots_;
thread_local std::vector<std::size_t> root_free_;

[[noreturn]] void fatal_oom() noexcept {
  std::abort();
}

namespace {

class Heap {
public:
  static Heap& instance() {
    static Heap h;
    return h;
  }

  void* acquire_block() {
    std::lock_guard<std::mutex> lock(mu_);
    void* block = std::aligned_alloc(block_alignment, block_size);
    if (block == nullptr) {
      fatal_oom();
    }
    blocks_.push_back(block);
    return block;
  }

private:
  std::mutex mu_;
  std::vector<void*> blocks_;
};

} // namespace

void* alloc_slow(std::size_t size) {
  auto* block = static_cast<std::byte*>(Heap::instance().acquire_block());
  Tlab& tlab = current_tlab;
  tlab.cursor = block + size;
  tlab.end = block + block_size;
  return block;
}

std::size_t register_root_slot(gc_ptr_base* p) noexcept {
  if (!root_free_.empty()) {
    std::size_t slot = root_free_.back();
    root_free_.pop_back();
    root_slots_[slot] = p;
    return slot;
  }
  std::size_t slot = root_slots_.size();
  root_slots_.push_back(p);
  return slot;
}

void unregister_root_slot(std::size_t slot) noexcept {
  root_slots_[slot] = nullptr;
  root_free_.push_back(slot);
}

void update_root_slot(std::size_t slot, gc_ptr_base* p) noexcept {
  root_slots_[slot] = p;
}

void visit_roots(Visitor& v) noexcept {
  for (gc_ptr_base* entry : root_slots_) {
    if (entry != nullptr) {
      v.visit(*entry);
    }
  }
}

} // namespace dustman::detail
