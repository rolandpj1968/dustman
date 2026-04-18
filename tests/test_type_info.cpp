#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/gc_ptr.hpp"
#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace {

struct Leaf {
  int id = 0;
};

struct Node {
  dustman::gc_ptr<Leaf> left;
  dustman::gc_ptr<Leaf> right;
};

int destruction_counter = 0;

struct WithDtor {
  ~WithDtor() noexcept { ++destruction_counter; }
};

class CollectingVisitor : public dustman::Visitor {
public:
  std::vector<void*> visited;
  void visit(dustman::gc_ptr_base& p) override { visited.push_back(p.load()); }
};

}  // namespace

template<>
struct dustman::Tracer<Node> : dustman::FieldList<Node, &Node::left, &Node::right> {};

template<>
struct dustman::Tracer<WithDtor> : dustman::FieldList<WithDtor> {};

TEST_CASE("TypeInfoFor reports correct size and alignment", "[type_info]") {
  constexpr const dustman::TypeInfo& ti = dustman::TypeInfoFor<Node>::value;
  REQUIRE(ti.size == sizeof(Node));
  REQUIRE(ti.align == alignof(Node));
}

TEST_CASE("TypeInfo.trace dispatches to the registered Tracer", "[type_info]") {
  Leaf la, lb;
  Node node;
  node.left = dustman::gc_ptr<Leaf>{&la};
  node.right = dustman::gc_ptr<Leaf>{&lb};

  const dustman::TypeInfo& ti = dustman::TypeInfoFor<Node>::value;

  CollectingVisitor v;
  void* erased = &node;
  ti.trace(erased, v);

  REQUIRE(v.visited.size() == 2);
  REQUIRE(v.visited[0] == &la);
  REQUIRE(v.visited[1] == &lb);
}

TEST_CASE("TypeInfo.destroy invokes the destructor", "[type_info]") {
  destruction_counter = 0;

  alignas(WithDtor) unsigned char storage[sizeof(WithDtor)];
  WithDtor* obj = new (storage) WithDtor{};

  const dustman::TypeInfo& ti = dustman::TypeInfoFor<WithDtor>::value;
  ti.destroy(obj);

  REQUIRE(destruction_counter == 1);
}

TEST_CASE("TypeInfo.flags defaults to zero", "[type_info]") {
  const dustman::TypeInfo& ti = dustman::TypeInfoFor<Node>::value;
  REQUIRE(ti.flags == 0);
}
