#include "dustman/heap.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "dustman/gc_ptr.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace dustman::detail {

thread_local Tlab small_tlab;
thread_local Tlab medium_tlab;

thread_local Generation alloc_target_gen_ = Generation::Young;
thread_local bool alloc_skip_recycle_ = false;
thread_local std::vector<BlockHeader*>* minor_evac_targets_ = nullptr;

struct ThreadRootSet {
  std::vector<gc_ptr_base*> slots;
  std::vector<std::size_t> free_slots;
};

thread_local ThreadRootSet my_roots_;

thread_local bool attached_ = false;
thread_local bool native_ = false;

namespace {

std::mutex stw_mu_;
std::condition_variable stw_cv_;
std::size_t attached_count_ = 0;
std::size_t parked_count_ = 0;
bool has_collector_ = false;

std::mutex roots_mu_;
std::vector<ThreadRootSet*> all_thread_roots_;

std::shared_mutex block_set_mu_;
std::unordered_set<std::uintptr_t> block_set_;

bool pause_is_set() noexcept {
  return pause_requested_.load(std::memory_order_acquire);
}

} // namespace

bool is_heap_block_base(std::uintptr_t base) noexcept {
  std::shared_lock<std::shared_mutex> lk(block_set_mu_);
  return block_set_.find(base) != block_set_.end();
}

void register_heap_block_base(std::uintptr_t base) noexcept {
  std::unique_lock<std::shared_mutex> lk(block_set_mu_);
  block_set_.insert(base);
}

void unregister_heap_block_base(std::uintptr_t base) noexcept {
  std::unique_lock<std::shared_mutex> lk(block_set_mu_);
  block_set_.erase(base);
}

void gc_write_barrier(void* slot) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(slot);
  auto base = addr & ~(block_alignment - 1);
  if (!is_heap_block_base(base)) return;
  auto* h = reinterpret_cast<BlockHeader*>(base);
  auto body = base + block_header_size;
  if (addr < body) return;
  auto card_idx = (addr - body) / line_size;
  if (card_idx < h->card_map.size()) {
    h->card_map[card_idx] = 1;
  }
}

[[noreturn]] void fatal_oom() noexcept {
  std::abort();
}

[[noreturn]] void fatal_reentrant_collect() noexcept {
  std::abort();
}

void safepoint_slow() noexcept {
  if (collecting_) return;
  if (!attached_) return;
  if (native_) return;
  small_tlab = {};
  medium_tlab = {};
  std::unique_lock<std::mutex> lk(stw_mu_);
  if (!pause_is_set()) return;
  ++parked_count_;
  stw_cv_.notify_all();
  stw_cv_.wait(lk, [] { return !pause_is_set(); });
  --parked_count_;
}

bool acquire_collector_slot() noexcept {
  std::unique_lock<std::mutex> lk(stw_mu_);
  if (has_collector_) {
    ++parked_count_;
    stw_cv_.notify_all();
    stw_cv_.wait(lk, [] { return !has_collector_; });
    --parked_count_;
    return false;
  }
  has_collector_ = true;
  pause_requested_.store(true, std::memory_order_release);
  stw_cv_.wait(lk, [] { return parked_count_ + 1 >= attached_count_; });
  return true;
}

void release_collector_slot() noexcept {
  std::unique_lock<std::mutex> lk(stw_mu_);
  has_collector_ = false;
  pause_requested_.store(false, std::memory_order_release);
  stw_cv_.notify_all();
}

} // namespace dustman::detail

namespace dustman {

void attach_thread() noexcept {
  if (detail::attached_) return;
  {
    std::lock_guard<std::mutex> rlk(detail::roots_mu_);
    detail::all_thread_roots_.push_back(&detail::my_roots_);
  }
  std::unique_lock<std::mutex> lk(detail::stw_mu_);
  ++detail::attached_count_;
  detail::attached_ = true;
  if (detail::pause_is_set()) {
    ++detail::parked_count_;
    detail::stw_cv_.notify_all();
    detail::stw_cv_.wait(lk, [] { return !detail::pause_is_set(); });
    --detail::parked_count_;
  }
}

void detach_thread() noexcept {
  if (!detail::attached_) return;
  detail::small_tlab = {};
  detail::medium_tlab = {};
  {
    std::unique_lock<std::mutex> lk(detail::stw_mu_);
    --detail::attached_count_;
    detail::attached_ = false;
    detail::stw_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> rlk(detail::roots_mu_);
    auto it = std::find(detail::all_thread_roots_.begin(),
                        detail::all_thread_roots_.end(),
                        &detail::my_roots_);
    if (it != detail::all_thread_roots_.end()) {
      detail::all_thread_roots_.erase(it);
    }
  }
}

void enter_native() noexcept {
  if (!detail::attached_) return;
  if (detail::native_) return;
  if (detail::collecting_) return;
  detail::small_tlab = {};
  detail::medium_tlab = {};
  std::unique_lock<std::mutex> lk(detail::stw_mu_);
  ++detail::parked_count_;
  detail::native_ = true;
  detail::stw_cv_.notify_all();
}

void leave_native() noexcept {
  if (!detail::native_) return;
  std::unique_lock<std::mutex> lk(detail::stw_mu_);
  detail::stw_cv_.wait(lk, [] { return !detail::pause_is_set(); });
  --detail::parked_count_;
  detail::native_ = false;
}

} // namespace dustman

namespace dustman::detail {

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

struct HugeRecord {
  void* base;
  void* hdr;
  std::size_t bytes;
  bool marked;
  bool updated;
};

class Heap {
public:
  static Heap& instance() {
    static Heap h;
    return h;
  }

  void* acquire_block(std::uint32_t flags, Generation gen) {
    std::lock_guard<std::mutex> lock(mu_);
    void* block = std::aligned_alloc(block_alignment, block_size);
    if (block == nullptr) {
      fatal_oom();
    }
    blocks_.push_back(block);
    auto* h = static_cast<BlockHeader*>(block);
    new (h) BlockHeader {};
    h->flags = flags;
    h->generation = gen;
    register_heap_block_base(reinterpret_cast<std::uintptr_t>(block));
    if (minor_evac_targets_ != nullptr && gen == Generation::Old) {
      minor_evac_targets_->push_back(h);
    }
    return block;
  }

  void* acquire_huge(std::size_t obj_bytes, std::size_t align) {
    const std::size_t alloc_align = (align > alignof(void*)) ? align : alignof(void*);
    std::size_t hdr_offset = 0;
    std::size_t alloc_size;
    if (align > alignof(void*)) {
      std::size_t body_size = obj_bytes - sizeof(const TypeInfo*);
      body_size = (body_size + align - 1) & ~(align - 1);
      alloc_size = align + body_size;
      hdr_offset = align - sizeof(const TypeInfo*);
    } else {
      alloc_size = (obj_bytes + alloc_align - 1) & ~(alloc_align - 1);
    }

    std::lock_guard<std::mutex> lock(mu_);
    void* base = std::aligned_alloc(alloc_align, alloc_size);
    if (base == nullptr) {
      fatal_oom();
    }
    void* hdr = static_cast<std::byte*>(base) + hdr_offset;
    huge_records_.push_back(HugeRecord {base, hdr, alloc_size, false, false});
    return hdr;
  }

  bool mark_huge(const void* body) noexcept {
    auto* hdr_ptr = reinterpret_cast<const std::byte*>(body) - sizeof(const TypeInfo*);
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& rec : huge_records_) {
      if (rec.hdr == hdr_ptr) {
        if (rec.marked)
          return false;
        rec.marked = true;
        return true;
      }
    }
    return false;
  }

  bool update_huge(const void* body) noexcept {
    auto* hdr_ptr = reinterpret_cast<const std::byte*>(body) - sizeof(const TypeInfo*);
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& rec : huge_records_) {
      if (rec.hdr == hdr_ptr) {
        if (rec.updated)
          return false;
        rec.updated = true;
        return true;
      }
    }
    return false;
  }

  void sweep_huge() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    auto new_end = std::remove_if(huge_records_.begin(), huge_records_.end(), [](HugeRecord& rec) {
      if (!rec.marked) {
        void* body = static_cast<std::byte*>(rec.hdr) + sizeof(const TypeInfo*);
        const TypeInfo* ti = type_of(body);
        ti->destroy(body);
        std::free(rec.base);
        return true;
      }
      rec.marked = false;
      rec.updated = false;
      return false;
    });
    huge_records_.erase(new_end, huge_records_.end());
  }

  std::size_t huge_count() {
    std::lock_guard<std::mutex> lock(mu_);
    return huge_records_.size();
  }

  void visit_all_huge(::dustman::Visitor& v) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& rec : huge_records_) {
      void* body = static_cast<std::byte*>(rec.hdr) + sizeof(const TypeInfo*);
      if (is_forwarded(body)) continue;
      const TypeInfo* ti = type_of(body);
      ti->trace(body, v);
    }
  }

  void free_specific_blocks_and_clear_all_cards(
      const std::vector<BlockHeader*>& to_free) noexcept;

  template <typename F>
  void for_each_block(F&& f) {
    std::lock_guard<std::mutex> lock(mu_);
    for (void* block : blocks_) {
      f(static_cast<BlockHeader*>(block));
    }
  }

  template <typename Pred>
  void remove_blocks_if_and_rebuild_recycle(Pred&& pred) {
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

  // Phase A: destroy fully-dead, compute line_map for densely-live,
  // flag sparsely-live and return them for evacuation.
  std::vector<BlockHeader*> classify_and_destroy_dead(std::uint32_t threshold_percent) noexcept;

  // Phase D: remove blocks flagged flag_block_evacuating, destroy any
  // remaining non-forwarded objects in them, rebuild recycle list.
  void finalize_sweep() noexcept;

private:
  std::mutex mu_;
  std::vector<void*> blocks_;
  std::vector<BlockHeader*> small_recycle_;
  std::vector<HugeRecord> huge_records_;
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

void destroy_non_forwarded_in(BlockHeader* h) noexcept {
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0)
      continue;

    auto* body_addr = body_start + slot * slot_bytes;
    if (is_forwarded(body_addr))
      continue;

    const TypeInfo* ti = type_of(body_addr);
    ti->destroy(body_addr);
  }
}

std::size_t compute_live_bytes(BlockHeader* h) noexcept {
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  std::size_t total = 0;
  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0)
      continue;
    if ((h->mark_bitmap[slot / 8] & mask) == 0)
      continue;

    auto* body_addr = body_start + slot * slot_bytes;
    const TypeInfo* ti = type_of(body_addr);
    total += object_bytes_of(ti);
  }
  return total;
}

void compute_line_map(BlockHeader* h) noexcept {
  h->line_map.fill(0);

  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0)
      continue;
    if ((h->mark_bitmap[slot / 8] & mask) == 0)
      continue;

    auto* body_addr = body_start + slot * slot_bytes;
    const TypeInfo* ti = type_of(body_addr);

    std::size_t alloc_bytes = object_bytes_of(ti);
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
  auto* block =
      static_cast<std::byte*>(Heap::instance().acquire_block(0, alloc_target_gen_));
  BlockHeader* h = reinterpret_cast<BlockHeader*>(block);
  return claim_line(h, 0, size);
}

void* alloc_fresh_medium_block(std::size_t size) {
  auto* block = static_cast<std::byte*>(
      Heap::instance().acquire_block(flag_block_medium, alloc_target_gen_));
  auto* body = block + block_header_size;

  medium_tlab.cursor = body + size;
  medium_tlab.end = block + block_size;
  return body;
}

} // namespace

std::vector<BlockHeader*>
Heap::classify_and_destroy_dead(std::uint32_t threshold_percent) noexcept {
  std::vector<BlockHeader*> sparse;
  std::lock_guard<std::mutex> lock(mu_);

  // Invalidate the recycle list before evacuation begins. Otherwise the
  // evacuation's own alloc_slow_small could pop a block just flagged for
  // evacuation, causing set_start / set_mark on the target to land in
  // the source's bitmaps and produce a self-iterating cascade.
  // finalize_sweep rebuilds the list from surviving blocks.
  small_recycle_.clear();

  auto new_end = std::remove_if(blocks_.begin(), blocks_.end(), [&](void* block) {
    auto* h = static_cast<BlockHeader*>(block);
    if (!block_has_any_mark(h)) {
      destroy_all_objects_in(h);
      unregister_heap_block_base(reinterpret_cast<std::uintptr_t>(block));
      return true;
    }
    std::size_t live = compute_live_bytes(h);
    std::size_t threshold_bytes =
        (static_cast<std::size_t>(threshold_percent) * block_body_size) / 100;
    if (live < threshold_bytes) {
      h->flags |= flag_block_evacuating;
      sparse.push_back(h);
    } else if (!is_medium_block(h)) {
      compute_line_map(h);
    }
    return false;
  });
  blocks_.erase(new_end, blocks_.end());
  return sparse;
}

void Heap::finalize_sweep() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  small_recycle_.clear();

  auto new_end = std::remove_if(blocks_.begin(), blocks_.end(), [](void* block) {
    auto* h = static_cast<BlockHeader*>(block);
    if ((h->flags & flag_block_evacuating) != 0) {
      destroy_non_forwarded_in(h);
      unregister_heap_block_base(reinterpret_cast<std::uintptr_t>(block));
      std::free(block);
      return true;
    }
    return false;
  });
  blocks_.erase(new_end, blocks_.end());

  for (void* block : blocks_) {
    auto* h = static_cast<BlockHeader*>(block);
    if (!is_medium_block(h) && h->generation == Generation::Young
        && block_has_any_free_line(h)) {
      small_recycle_.push_back(h);
    }
  }
}

void Heap::free_specific_blocks_and_clear_all_cards(
    const std::vector<BlockHeader*>& to_free) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  small_recycle_.clear();
  std::unordered_set<BlockHeader*> set(to_free.begin(), to_free.end());
  auto new_end = std::remove_if(blocks_.begin(), blocks_.end(), [&set](void* block) {
    auto* h = static_cast<BlockHeader*>(block);
    if (set.find(h) != set.end()) {
      destroy_non_forwarded_in(h);
      unregister_heap_block_base(reinterpret_cast<std::uintptr_t>(block));
      std::free(block);
      return true;
    }
    return false;
  });
  blocks_.erase(new_end, blocks_.end());
  for (void* block : blocks_) {
    auto* h = static_cast<BlockHeader*>(block);
    h->card_map.fill(0);
    if (!is_medium_block(h) && h->generation == Generation::Young
        && block_has_any_free_line(h)) {
      small_recycle_.push_back(h);
    }
  }
}

void evacuate_block(BlockHeader* h) {
  const bool medium = is_medium_block(h);
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;

  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0)
      continue;
    if ((h->mark_bitmap[slot / 8] & mask) == 0)
      continue;

    auto* old_body = body_start + slot * slot_bytes;
    const TypeInfo* ti = type_of(old_body);
    std::size_t alloc_bytes = object_bytes_of(ti);

    void* new_hdr;
    if (medium) {
      new_hdr = tlab_bump(medium_tlab, alloc_bytes);
      if (new_hdr == nullptr)
        new_hdr = alloc_slow_medium(alloc_bytes);
    } else {
      new_hdr = tlab_bump(small_tlab, alloc_bytes);
      if (new_hdr == nullptr)
        new_hdr = alloc_slow_small(alloc_bytes);
    }

    *static_cast<const TypeInfo**>(new_hdr) = ti;
    void* new_body = static_cast<std::byte*>(new_hdr) + sizeof(const TypeInfo*);
    std::memcpy(new_body, old_body, ti->size);
    set_start(new_body);
    set_mark(new_body);
    set_forwarded(old_body, new_body);
  }
}

void* alloc_slow_small(std::size_t size) {
  ensure_attached();
  safepoint();
  BlockHeader* current = (small_tlab.cursor != nullptr) ? header_of(small_tlab.cursor) : nullptr;

  if (current != nullptr) {
    std::size_t start_line = line_of(small_tlab.cursor) + 1;
    std::size_t line = find_free_line_in(current, start_line);
    if (line != npos) {
      return claim_line(current, line, size);
    }
  }

  if (!alloc_skip_recycle_) {
    BlockHeader* recycled = Heap::instance().pop_small_recycled();
    if (recycled != nullptr) {
      std::size_t line = find_free_line_in(recycled, 0);
      return claim_line(recycled, line, size);
    }
  }

  return alloc_fresh_small_block(size);
}

void* alloc_slow_medium(std::size_t size) {
  ensure_attached();
  safepoint();
  return alloc_fresh_medium_block(size);
}

void* alloc_huge(std::size_t obj_bytes, std::size_t align) {
  ensure_attached();
  safepoint();
  return Heap::instance().acquire_huge(obj_bytes, align);
}

bool mark_huge(const void* body) noexcept {
  return Heap::instance().mark_huge(body);
}

bool update_huge(const void* body) noexcept {
  return Heap::instance().update_huge(body);
}

void sweep_huge() noexcept {
  Heap::instance().sweep_huge();
}

std::size_t huge_count() noexcept {
  return Heap::instance().huge_count();
}

std::vector<BlockHeader*> classify_and_destroy_dead(std::uint32_t threshold_percent) noexcept {
  return Heap::instance().classify_and_destroy_dead(threshold_percent);
}

void finalize_sweep() noexcept {
  Heap::instance().finalize_sweep();
}

void clear_all_marks() noexcept {
  Heap::instance().for_each_block([](BlockHeader* h) { h->mark_bitmap.fill(0); });
}

std::vector<BlockHeader*> collect_young_blocks() noexcept {
  std::vector<BlockHeader*> out;
  Heap::instance().for_each_block([&out](BlockHeader* h) {
    if (h->generation == Generation::Young) {
      out.push_back(h);
    }
  });
  return out;
}

void visit_block_live_objects(BlockHeader* h, ::dustman::Visitor& v) noexcept {
  auto* block_base = reinterpret_cast<std::byte*>(h);
  auto* body_start = block_base + block_header_size;
  for (std::size_t slot = 0; slot < max_slots_per_block; ++slot) {
    std::uint8_t mask = std::uint8_t(1) << (slot % 8);
    if ((h->start_bitmap[slot / 8] & mask) == 0) continue;
    if ((h->mark_bitmap[slot / 8] & mask) == 0) continue;
    auto* body_addr = body_start + slot * slot_bytes;
    if (is_forwarded(body_addr)) continue;
    const TypeInfo* ti = type_of(body_addr);
    ti->trace(body_addr, v);
  }
}

void visit_dirty_cards_of_old_blocks(::dustman::Visitor& v) noexcept {
  Heap::instance().for_each_block([&v](BlockHeader* h) {
    if (h->generation != Generation::Old) return;
    bool any_dirty = false;
    for (std::size_t i = 0; i < h->card_map.size(); ++i) {
      if (h->card_map[i] != 0) { any_dirty = true; break; }
    }
    if (!any_dirty) return;
    auto* block_base = reinterpret_cast<std::byte*>(h);
    auto* body_start = block_base + block_header_size;
    for (std::size_t line = 0; line < lines_per_block; ++line) {
      if (h->card_map[line] == 0) continue;
      std::size_t slot_lo = (line * line_size) / slot_bytes;
      std::size_t slot_hi = ((line + 1) * line_size) / slot_bytes;
      if (slot_hi > max_slots_per_block) slot_hi = max_slots_per_block;
      for (std::size_t slot = slot_lo; slot < slot_hi; ++slot) {
        std::uint8_t mask = std::uint8_t(1) << (slot % 8);
        if ((h->start_bitmap[slot / 8] & mask) == 0) continue;
        if ((h->mark_bitmap[slot / 8] & mask) == 0) continue;
        auto* body_addr = body_start + slot * slot_bytes;
        if (is_forwarded(body_addr)) continue;
        const TypeInfo* ti = type_of(body_addr);
        ti->trace(body_addr, v);
      }
    }
  });
}

void visit_all_huge(::dustman::Visitor& v) noexcept {
  Heap::instance().visit_all_huge(v);
}

void free_young_blocks_and_clear_cards(const std::vector<BlockHeader*>& youngs) noexcept {
  Heap::instance().free_specific_blocks_and_clear_all_cards(youngs);
}

std::size_t heap_block_count() noexcept {
  return Heap::instance().size();
}

std::size_t register_root_slot(gc_ptr_base* p) noexcept {
  ensure_attached();
  if (!my_roots_.free_slots.empty()) {
    std::size_t slot = my_roots_.free_slots.back();
    my_roots_.free_slots.pop_back();
    my_roots_.slots[slot] = p;
    return slot;
  }
  std::size_t slot = my_roots_.slots.size();
  my_roots_.slots.push_back(p);
  return slot;
}

void unregister_root_slot(std::size_t slot) noexcept {
  my_roots_.slots[slot] = nullptr;
  my_roots_.free_slots.push_back(slot);
}

void update_root_slot(std::size_t slot, gc_ptr_base* p) noexcept {
  my_roots_.slots[slot] = p;
}

void visit_roots(Visitor& v) noexcept {
  std::lock_guard<std::mutex> lk(roots_mu_);
  for (ThreadRootSet* trs : all_thread_roots_) {
    for (gc_ptr_base* entry : trs->slots) {
      if (entry != nullptr) {
        v.visit(*entry);
      }
    }
  }
}

} // namespace dustman::detail
