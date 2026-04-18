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

} // namespace
} // namespace detail

void collect() noexcept {
  if (detail::collecting_) {
    detail::fatal_reentrant_collect();
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
}

} // namespace dustman
