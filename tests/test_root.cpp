#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/visitor.hpp"

namespace {

struct Foo {
  int v = 0;
};

class CollectingVisitor : public dustman::Visitor {
public:
  std::vector<void*> visited;
  void visit(dustman::gc_ptr_base& p) override { visited.push_back(p.load()); }
};

std::vector<void*> snapshot_roots() {
  CollectingVisitor v;
  dustman::detail::visit_roots(v);
  return v.visited;
}

bool contains(const std::vector<void*>& xs, void* target) {
  for (void* p : xs) {
    if (p == target)
      return true;
  }
  return false;
}

} // namespace

TEST_CASE("default-constructed Root is a null registered root", "[root]") {
  const auto before = snapshot_roots().size();
  {
    dustman::Root<Foo> r;
    REQUIRE(r.get() == nullptr);
    REQUIRE(static_cast<bool>(r) == false);
    REQUIRE(snapshot_roots().size() == before + 1);
  }
  REQUIRE(snapshot_roots().size() == before);
}

TEST_CASE("Root constructed from gc_ptr reports the pointed address", "[root]") {
  Foo foo;
  const auto before = snapshot_roots().size();
  {
    dustman::Root<Foo> r {dustman::gc_ptr<Foo> {&foo}};
    REQUIRE(r.get() == &foo);
    REQUIRE(static_cast<bool>(r));

    const auto during = snapshot_roots();
    REQUIRE(during.size() == before + 1);
    REQUIRE(contains(during, &foo));
  }
  REQUIRE(snapshot_roots().size() == before);
}

TEST_CASE("Root assignment from gc_ptr updates payload without churning slot", "[root]") {
  Foo a, b;
  dustman::Root<Foo> r;
  const auto root_count = snapshot_roots().size();

  REQUIRE(r.get() == nullptr);

  r = dustman::gc_ptr<Foo> {&a};
  REQUIRE(r.get() == &a);
  REQUIRE(snapshot_roots().size() == root_count);

  r = dustman::gc_ptr<Foo> {&b};
  REQUIRE(r.get() == &b);
  REQUIRE(snapshot_roots().size() == root_count);

  r = nullptr;
  REQUIRE(r.get() == nullptr);
  REQUIRE(snapshot_roots().size() == root_count);
}

TEST_CASE("Root move constructor transfers registration", "[root]") {
  Foo foo;
  const auto before = snapshot_roots().size();

  dustman::Root<Foo> r1 {dustman::gc_ptr<Foo> {&foo}};
  REQUIRE(snapshot_roots().size() == before + 1);

  dustman::Root<Foo> r2 {std::move(r1)};
  REQUIRE(r2.get() == &foo);

  const auto during = snapshot_roots();
  REQUIRE(during.size() == before + 1);
  REQUIRE(contains(during, &foo));
}

TEST_CASE("Root move assignment unregisters receiver and takes source's slot", "[root]") {
  Foo a, b;
  const auto before = snapshot_roots().size();

  dustman::Root<Foo> r1 {dustman::gc_ptr<Foo> {&a}};
  dustman::Root<Foo> r2 {dustman::gc_ptr<Foo> {&b}};
  REQUIRE(snapshot_roots().size() == before + 2);

  r1 = std::move(r2);

  const auto during = snapshot_roots();
  REQUIRE(during.size() == before + 1);
  REQUIRE(r1.get() == &b);
  REQUIRE(contains(during, &b));
  REQUIRE_FALSE(contains(during, &a));
}

TEST_CASE("Roots survive std::vector reallocation", "[root]") {
  Foo foos[5];
  const auto before = snapshot_roots().size();

  std::vector<dustman::Root<Foo>> roots;
  for (Foo& f : foos) {
    roots.emplace_back(dustman::gc_ptr<Foo> {&f});
  }

  for (std::size_t i = 0; i < 5; ++i) {
    REQUIRE(roots[i].get() == &foos[i]);
  }

  const auto during = snapshot_roots();
  REQUIRE(during.size() == before + 5);
  for (Foo& f : foos) {
    REQUIRE(contains(during, &f));
  }
}

TEST_CASE("Root implicitly converts to gc_ptr for field assignment", "[root]") {
  Foo foo;
  dustman::Root<Foo> r {dustman::gc_ptr<Foo> {&foo}};

  dustman::gc_ptr<Foo> gp = r;
  REQUIRE(gp.get() == &foo);
  REQUIRE(r.get() == &foo);
}

TEST_CASE("Root access surface matches gc_ptr", "[root]") {
  Foo foo;
  foo.v = 42;
  dustman::Root<Foo> r {dustman::gc_ptr<Foo> {&foo}};

  REQUIRE(r.get() == &foo);
  REQUIRE(r->v == 42);
  REQUIRE((*r).v == 42);
  REQUIRE(static_cast<bool>(r));

  r = nullptr;
  REQUIRE(r.get() == nullptr);
  REQUIRE(static_cast<bool>(r) == false);
}

TEST_CASE("free slots are reused after unregistration", "[root]") {
  const auto before = snapshot_roots().size();

  for (int i = 0; i < 64; ++i) {
    dustman::Root<Foo> r;
    REQUIRE(snapshot_roots().size() == before + 1);
  }

  REQUIRE(snapshot_roots().size() == before);
}
