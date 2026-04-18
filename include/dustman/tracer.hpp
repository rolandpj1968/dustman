#pragma once

#include "dustman/visitor.hpp"

namespace dustman {

template<typename T>
struct Tracer;

template<typename T, auto... Ms>
struct FieldList {
  static void trace(T& obj, Visitor& v) {
    (v.visit(obj.*Ms), ...);
  }
};

}  // namespace dustman
