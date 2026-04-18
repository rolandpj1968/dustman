#include <catch2/catch_test_macros.hpp>

#include "dustman/gc_ptr.hpp"

namespace {

struct Foo {
  int v = 0;
};

} // namespace

TEST_CASE("gc_ptr is null by default", "[gc_ptr]") {
  dustman::gc_ptr<Foo> p;
  REQUIRE(p.get() == nullptr);
  REQUIRE(static_cast<bool>(p) == false);
  REQUIRE(p == nullptr);
  REQUIRE(nullptr == p);
}

TEST_CASE("gc_ptr constructed from nullptr is null", "[gc_ptr]") {
  dustman::gc_ptr<Foo> p {nullptr};
  REQUIRE(p == nullptr);
}

TEST_CASE("gc_ptr constructed from T* stores the address", "[gc_ptr]") {
  Foo foo {42};
  dustman::gc_ptr<Foo> p {&foo};
  REQUIRE(p.get() == &foo);
  REQUIRE(p != nullptr);
  REQUIRE(static_cast<bool>(p));
  REQUIRE(p->v == 42);
  REQUIRE((*p).v == 42);
}

TEST_CASE("gc_ptr copy semantics preserve the pointee", "[gc_ptr]") {
  Foo foo {7};
  dustman::gc_ptr<Foo> a {&foo};
  dustman::gc_ptr<Foo> b = a;
  REQUIRE(b.get() == &foo);
  REQUIRE(a == b);

  dustman::gc_ptr<Foo> c;
  c = a;
  REQUIRE(c.get() == &foo);
}

TEST_CASE("gc_ptr nullptr assignment resets", "[gc_ptr]") {
  Foo foo;
  dustman::gc_ptr<Foo> p {&foo};
  REQUIRE(p != nullptr);
  p = nullptr;
  REQUIRE(p == nullptr);
}

TEST_CASE("gc_ptr comparison detects distinct pointees", "[gc_ptr]") {
  Foo a, b;
  dustman::gc_ptr<Foo> pa {&a};
  dustman::gc_ptr<Foo> pb {&b};
  REQUIRE(pa != pb);
  REQUIRE_FALSE(pa == pb);
}

TEST_CASE("gc_ptr is accessible through gc_ptr_base", "[gc_ptr]") {
  Foo foo;
  dustman::gc_ptr<Foo> p {&foo};
  dustman::gc_ptr_base& base = p;
  REQUIRE(base.load() == &foo);

  Foo other;
  base.store(&other);
  REQUIRE(p.get() == &other);
}
