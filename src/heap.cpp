#include "dustman/heap.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

#include "dustman/gc_ptr.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace dustman::detail {

thread_local Tlab small_tlab;
thread_local Tlab medium_tlab;

thread_local std::vector<gc_ptr_base*> root_slots_;
thread_local std::vector<std::size_t> root_free_;

[[noreturn]] void fatal_oom() noexcept {
  std::abort();
}

[[noreturn]] void fatal_reentrant_collect() noexcept {
  std::abort();
}

namespace {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

std::size_t find_free_line_in(const BlockHeader* h, std::size_t start_line) noexcept {
  for (std::size_t line = start_line; line < lines_per_block; ++line) {
    if (h->line_map[line] == 0)
      return line;
  }
  return npos;
}

bool block_has_any_free_line(const BlockHeader* h) noexcept {
  return find_free_line_in(h, 0) != npos;
}

class Heap {
public:
  static Heap& instance() {
    static Heap h;
    return h;
  }

  void* acquire_block(std::uint32_t flags) {
    std::lock_guard<std::mutex> lock(mu_);
    void* block = std::aligned_alloc(block_alignment, block_size);
    if (block == nullptr) {
      fatal_oom();
    }
    blocks_.push_back(block);
    auto* h = static_cast<BlockHeader*>(block);
    new (h) BlockHeader {};
    h->flags = flags;
    return block;
  }

  template <typename F>
  void for_each_block(F&& f) {
    std::lock_guard<std::mutex> lock(mu_);
    for (void* block : blocks_) {
      f(static_cast<BlockHeader*>(block));
    }
  }

  template <typename Pred>
  void remove_blocks_if(Pred&& pred) {
    std::lock_guard<std::mutex> lock(mu_);
    small_recycle_.clear();
    auto new_end = std::remove_if(blocks_.begin(), blocks_.end(), [&pred](void* block) {
      auto* h = static_cast<BlockHeader*>(block);
      if (pred(h)) {
        std::free(block);
        return true;
      }
      return false;
    });
    blocks_.erase(new_end, blocks_.end());
    for (void* block : blocks_) {
      auto* h = static_cast<BlockHeader*>(block);
      if (!is_medium_block(h) && block_has_any_free_line(h)) {
        small_recycle_.push_back(h);
      }
    }
  }

  BlockHeader* pop_small_recycled() {
    std::lock_guard<std::mutex> lock(mu_);
    while (!small_recycle_.empty()) {
      BlockHeader* h = small_recycle_.back();
      small_recycle_.pop_back();
      if (block_has_any_free_line(h)) {
        return h;
      }
    }
    return nullptr;
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return blocks_.size();
  }

private:
  std::mutex mu_;
  std::vector<void*> blocks_;
  std::vector<BlockHeader*> small_recycle_;
};

bool block_has_any_mark(BlockHeader* h) noexcept {
  for (std::uint8_t byte : h->mark_bitmap) {
    if (byte != 0)
      return true;
  }
  return false;
}

void destroy_all_objects_in(BlockHeader* h) noexcept {
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0)
      continue;

    auto* body_addr = body_start + slot * slot_bytes;
    const TypeInfo* ti = type_of(body_addr);
    ti->destroy(body_addr);
  }
}

void compute_line_map(BlockHeader* h) noexcept {
  h->line_map.fill(0);

  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    bool start_bit = (h->start_bitmap[slot / 8] & mask) != 0;
    if (!start_bit)
      continue;
    bool mark_bit = (h->mark_bitmap[slot / 8] & mask) != 0;
    if (!mark_bit)
      continue;

    auto* body_addr = body_start + slot * slot_bytes;
    const TypeInfo* ti = type_of(body_addr);

    std::size_t alloc_bytes = sizeof(const TypeInfo*) + ti->size;
    alloc_bytes = (alloc_bytes + slot_bytes - 1) & ~(slot_bytes - 1);

    std::size_t header_offset = (slot - 1) * slot_bytes;
    std::size_t alloc_end_offset = header_offset + alloc_bytes;

    std::size_t first_line = header_offset / line_size;
    std::size_t last_line = (alloc_end_offset - 1) / line_size;
    if (last_line >= lines_per_block) {
      last_line = lines_per_block - 1;
    }

    for (std::size_t line = first_line; line <= last_line; ++line) {
      h->line_map[line] = 1;
    }
  }
}

void* claim_line(BlockHeader* h, std::size_t line, std::size_t size) noexcept {
  h->line_map[line] = 1;
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;
  auto* line_start = body_start + line * line_size;

  small_tlab.cursor = line_start + size;
  small_tlab.end = line_start + line_size;
  return line_start;
}

void* alloc_fresh_small_block(std::size_t size) {
  auto* block = static_cast<std::byte*>(Heap::instance().acquire_block(0));
  BlockHeader* h = reinterpret_cast<BlockHeader*>(block);
  return claim_line(h, 0, size);
}

void* alloc_fresh_medium_block(std::size_t size) {
  auto* block = static_cast<std::byte*>(Heap::instance().acquire_block(flag_block_medium));
  auto* body = block + block_header_size;

  medium_tlab.cursor = body + size;
  medium_tlab.end = block + block_size;
  return body;
}

} // namespace

void* alloc_slow_small(std::size_t size) {
  BlockHeader* current = (small_tlab.cursor != nullptr) ? header_of(small_tlab.cursor) : nullptr;

  if (current != nullptr) {
    std::size_t start_line = line_of(small_tlab.cursor) + 1;
    std::size_t line = find_free_line_in(current, start_line);
    if (line != npos) {
      return claim_line(current, line, size);
    }
  }

  BlockHeader* recycled = Heap::instance().pop_small_recycled();
  if (recycled != nullptr) {
    std::size_t line = find_free_line_in(recycled, 0);
    return claim_line(recycled, line, size);
  }

  return alloc_fresh_small_block(size);
}

void* alloc_slow_medium(std::size_t size) {
  return alloc_fresh_medium_block(size);
}

void clear_all_marks() noexcept {
  Heap::instance().for_each_block([](BlockHeader* h) { h->mark_bitmap.fill(0); });
}

void sweep_all_blocks() noexcept {
  Heap::instance().remove_blocks_if([](BlockHeader* h) {
    if (!block_has_any_mark(h)) {
      destroy_all_objects_in(h);
      return true;
    }
    if (!is_medium_block(h)) {
      compute_line_map(h);
    }
    return false;
  });
}

std::size_t heap_block_count() noexcept {
  return Heap::instance().size();
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
