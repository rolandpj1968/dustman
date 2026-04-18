#include "dustman/heap.hpp"

#include <cstdlib>
#include <mutex>
#include <vector>

namespace dustman::detail {

thread_local Tlab current_tlab;

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

} // namespace dustman::detail
