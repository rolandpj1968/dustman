#pragma once

#include "dustman/gc_ptr.hpp"

namespace dustman {

class Visitor {
public:
  Visitor() = default;
  Visitor(const Visitor&) = delete;
  Visitor& operator=(const Visitor&) = delete;
  virtual ~Visitor() = default;

  virtual void visit(gc_ptr_base& p) = 0;
};

}  // namespace dustman
