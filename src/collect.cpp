#include "dustman/collect.hpp"

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

  detail::small_tlab.cursor = nullptr;
  detail::small_tlab.end = nullptr;
  detail::medium_tlab.cursor = nullptr;
  detail::medium_tlab.end = nullptr;

  detail::sweep_all_blocks();

  detail::gc_state = detail::GcState::idle;
  detail::collecting_ = false;
}

} // namespace dustman
