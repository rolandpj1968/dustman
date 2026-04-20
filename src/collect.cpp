#include "dustman/collect.hpp"

#include <unordered_set>
#include <vector>

#include "dustman/alloc.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace dustman {

namespace detail {

thread_local bool collecting_ = false;

namespace {

class MarkVisitor : public Visitor {
public:
  void visit(gc_ptr_base& p) override {
    void* obj = p.load();
    if (obj == nullptr) {
      return;
    }
    const TypeInfo* ti = type_of(obj);
    if ((ti->flags & flag_huge) != 0) {
      if (mark_huge(obj)) {
        worklist_.push_back(obj);
      }
      return;
    }
    if (is_marked(obj)) {
      return;
    }
    set_mark(obj);
    worklist_.push_back(obj);
  }

  void drain() {
    while (!worklist_.empty()) {
      void* obj = worklist_.back();
      worklist_.pop_back();
      const TypeInfo* ti = type_of(obj);
      ti->trace(obj, *this);
    }
  }

private:
  std::vector<void*> worklist_;
};

class UpdateVisitor : public Visitor {
public:
  void visit(gc_ptr_base& p) override {
    void* obj = p.load();
    if (obj == nullptr) {
      return;
    }
    if (is_forwarded(obj)) {
      obj = forwarded_to(obj);
      p.store(obj);
    }

    const TypeInfo* ti = type_of(obj);
    if ((ti->flags & flag_huge) != 0) {
      if (update_huge(obj)) {
        worklist_.push_back(obj);
      }
      return;
    }

    if (!visited_.insert(obj).second) {
      return;
    }
    worklist_.push_back(obj);
  }

  void drain() {
    while (!worklist_.empty()) {
      void* obj = worklist_.back();
      worklist_.pop_back();
      const TypeInfo* ti = type_of(obj);
      ti->trace(obj, *this);
    }
  }

private:
  std::vector<void*> worklist_;
  std::unordered_set<void*> visited_;
};

class MinorMarkVisitor : public Visitor {
public:
  void visit(gc_ptr_base& p) override {
    void* obj = p.load();
    if (obj == nullptr) return;
    const TypeInfo* ti = type_of(obj);
    if ((ti->flags & flag_huge) != 0) return;
    BlockHeader* h = header_of(obj);
    if (h->generation != Generation::Young) return;
    if (is_marked(obj)) return;
    set_mark(obj);
    worklist_.push_back(obj);
  }

  void drain() {
    while (!worklist_.empty()) {
      void* obj = worklist_.back();
      worklist_.pop_back();
      const TypeInfo* ti = type_of(obj);
      ti->trace(obj, *this);
    }
  }

private:
  std::vector<void*> worklist_;
};

class MinorUpdateVisitor : public Visitor {
public:
  void visit(gc_ptr_base& p) override {
    void* obj = p.load();
    if (obj == nullptr) return;
    if (is_forwarded(obj)) {
      p.store(forwarded_to(obj));
    }
  }
};

} // namespace
} // namespace detail

void collect() noexcept {
  if (detail::collecting_) {
    detail::fatal_reentrant_collect();
  }

  detail::ensure_attached();

  if (!detail::acquire_collector_slot()) {
    return;
  }

  detail::collecting_ = true;
  detail::gc_state = detail::GcState::marking;

  detail::clear_all_marks();

  detail::MarkVisitor mv;
  detail::visit_roots(mv);
  mv.drain();

  detail::gc_state = detail::GcState::sweeping;

  detail::small_tlab = {};
  detail::medium_tlab = {};

  std::uint32_t threshold = get_evacuation_threshold_percent();
  auto sparse = detail::classify_and_destroy_dead(threshold);

  for (auto* h : sparse) {
    detail::evacuate_block(h);
  }

  detail::UpdateVisitor uv;
  detail::visit_roots(uv);
  uv.drain();

  detail::small_tlab = {};
  detail::medium_tlab = {};

  detail::finalize_sweep();
  detail::sweep_huge();

  detail::gc_state = detail::GcState::idle;
  detail::collecting_ = false;
  detail::release_collector_slot();
}

void minor_collect() noexcept {
  if (detail::collecting_) {
    detail::fatal_reentrant_collect();
  }

  detail::ensure_attached();

  if (!detail::acquire_collector_slot()) {
    return;
  }

  detail::collecting_ = true;
  detail::gc_state = detail::GcState::marking;

  auto young_blocks = detail::collect_young_blocks();
  for (auto* h : young_blocks) {
    h->mark_bitmap.fill(0);
  }

  detail::MinorMarkVisitor mv;
  detail::visit_roots(mv);
  detail::visit_dirty_cards_of_old_blocks(mv);
  detail::visit_all_huge(mv);
  mv.drain();

  detail::gc_state = detail::GcState::sweeping;

  detail::small_tlab = {};
  detail::medium_tlab = {};
  detail::alloc_target_gen_ = detail::Generation::Old;
  detail::alloc_skip_recycle_ = true;

  std::vector<detail::BlockHeader*> evac_targets;
  detail::minor_evac_targets_ = &evac_targets;

  for (auto* h : young_blocks) {
    detail::evacuate_block(h);
  }

  detail::minor_evac_targets_ = nullptr;
  detail::alloc_skip_recycle_ = false;
  detail::alloc_target_gen_ = detail::Generation::Young;
  detail::small_tlab = {};
  detail::medium_tlab = {};

  detail::MinorUpdateVisitor uv;
  detail::visit_roots(uv);
  detail::visit_dirty_cards_of_old_blocks(uv);
  detail::visit_all_huge(uv);
  for (auto* h : evac_targets) {
    detail::visit_block_live_objects(h, uv);
  }

  detail::free_young_blocks_and_clear_cards(young_blocks);

  detail::gc_state = detail::GcState::idle;
  detail::collecting_ = false;
  detail::release_collector_slot();
}

} // namespace dustman
