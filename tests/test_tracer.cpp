#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/gc_ptr.hpp"
#include "dustman/tracer.hpp"
#include "dustman/visitor.hpp"

namespace {

struct Leaf {
  int id = 0;
};

struct TwoFields {
  dustman::gc_ptr<Leaf> a;
  dustman::gc_ptr<Leaf> b;
};

struct ThreeFields {
  dustman::gc_ptr<Leaf> x;
  dustman::gc_ptr<Leaf> y;
  dustman::gc_ptr<Leaf> z;
};

struct NoFields {};

struct HandRolled {
  dustman::gc_ptr<Leaf> first;
  int payload = 0;
  dustman::gc_ptr<Leaf> second;
};

class CollectingVisitor : public dustman::Visitor {
public:
  std::vector<void*> visited;
  void visit(dustman::gc_ptr_base& p) override { visited.push_back(p.load()); }
};

} // namespace

template <>
struct dustman::Tracer<TwoFields> : dustman::FieldList<TwoFields, &TwoFields::a, &TwoFields::b> {};

template <>
struct dustman::Tracer<ThreeFields>
    : dustman::FieldList<ThreeFields, &ThreeFields::x, &ThreeFields::y, &ThreeFields::z> {};

template <>
struct dustman::Tracer<NoFields> : dustman::FieldList<NoFields> {};

template <>
struct dustman::Tracer<HandRolled> {
  static void trace(HandRolled& obj, dustman::Visitor& v) {
    v.visit(obj.first);
    v.visit(obj.second);
  }
};

TEST_CASE("FieldList visits every declared field in order", "[tracer]") {
  Leaf la {1}, lb {2};
  TwoFields obj;
  obj.a = dustman::gc_ptr<Leaf> {&la};
  obj.b = dustman::gc_ptr<Leaf> {&lb};

  CollectingVisitor v;
  dustman::Tracer<TwoFields>::trace(obj, v);

  REQUIRE(v.visited.size() == 2);
  REQUIRE(v.visited[0] == &la);
  REQUIRE(v.visited[1] == &lb);
}

TEST_CASE("FieldList handles more than two fields", "[tracer]") {
  Leaf a {1}, b {2}, c {3};
  ThreeFields obj;
  obj.x = dustman::gc_ptr<Leaf> {&a};
  obj.y = dustman::gc_ptr<Leaf> {&b};
  obj.z = dustman::gc_ptr<Leaf> {&c};

  CollectingVisitor v;
  dustman::Tracer<ThreeFields>::trace(obj, v);

  REQUIRE(v.visited.size() == 3);
  REQUIRE(v.visited[0] == &a);
  REQUIRE(v.visited[1] == &b);
  REQUIRE(v.visited[2] == &c);
}

TEST_CASE("Empty FieldList visits nothing", "[tracer]") {
  NoFields obj;
  CollectingVisitor v;
  dustman::Tracer<NoFields>::trace(obj, v);
  REQUIRE(v.visited.empty());
}

TEST_CASE("Hand-written Tracer skips non-gc fields", "[tracer]") {
  Leaf la {10}, lb {20};
  HandRolled obj;
  obj.first = dustman::gc_ptr<Leaf> {&la};
  obj.payload = 99;
  obj.second = dustman::gc_ptr<Leaf> {&lb};

  CollectingVisitor v;
  dustman::Tracer<HandRolled>::trace(obj, v);

  REQUIRE(v.visited.size() == 2);
  REQUIRE(v.visited[0] == &la);
  REQUIRE(v.visited[1] == &lb);
}

TEST_CASE("FieldList yields a null pointer for an unset gc_ptr", "[tracer]") {
  TwoFields obj;
  CollectingVisitor v;
  dustman::Tracer<TwoFields>::trace(obj, v);

  REQUIRE(v.visited.size() == 2);
  REQUIRE(v.visited[0] == nullptr);
  REQUIRE(v.visited[1] == nullptr);
}
