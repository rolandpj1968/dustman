#include "dustman/heap.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "dustman/gc_ptr.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace dustman::detail {

thread_local Tlab current_tlab;

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
    auto new_end = std::remove_if(blocks_.begin(), blocks_.end(), [&pred](void* block) {
      auto* h = static_cast<BlockHeader*>(block);
      if (pred(h)) {
        std::free(block);
        return true;
      }
      return false;
    });
    blocks_.erase(new_end, blocks_.end());
  }

  std::pair<BlockHeader*, std::size_t> find_free_line(const BlockHeader* exclude) {
    std::lock_guard<std::mutex> lock(mu_);
    for (void* block : blocks_) {
      auto* h = static_cast<BlockHeader*>(block);
      if (h == exclude)
        continue;
      std::size_t line = find_free_line_in(h, 0);
      if (line != npos)
        return {h, line};
    }
    return {nullptr, npos};
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return blocks_.size();
  }

private:
  std::mutex mu_;
  std::vector<void*> blocks_;
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

  Tlab& tlab = current_tlab;
  tlab.cursor = line_start + size;
  tlab.line_end = line_start + line_size;
  return line_start;
}

void* alloc_slow_fresh_block(std::size_t size) {
  auto* block = static_cast<std::byte*>(Heap::instance().acquire_block());
  new (block) BlockHeader {};
  BlockHeader* h = reinterpret_cast<BlockHeader*>(block);
  return claim_line(h, 0, size);
}

} // namespace

void* alloc_slow(std::size_t size) {
  Tlab& tlab = current_tlab;

  BlockHeader* current_block = (tlab.cursor != nullptr) ? header_of(tlab.cursor) : nullptr;

  if (current_block != nullptr) {
    std::size_t start_line = line_of(tlab.cursor) + 1;
    std::size_t line = find_free_line_in(current_block, start_line);
    if (line != npos) {
      return claim_line(current_block, line, size);
    }
  }

  return alloc_slow_fresh_block(size);
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
    compute_line_map(h);
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
