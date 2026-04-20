#include <atomic>
#include <cstddef>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "dustman/alloc.hpp"
#include "dustman/collect.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/heap.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"

namespace {

struct MLeaf {
  int v = 0;
};

struct MNode {
  dustman::gc_ptr<MLeaf> leaf;
  int payload = 0;
};

struct MCyclic {
  dustman::gc_ptr<MCyclic> next;
  int tag = 0;
};

inline int m_leaf_dtor_count = 0;
struct MCountedLeaf {
  int v = 0;
  ~MCountedLeaf() { ++m_leaf_dtor_count; }
};

struct alignas(4096) MHugeWithRef {
  dustman::gc_ptr<MLeaf> leaf;
  char fill[8192] {};
};

} // namespace

template <>
struct dustman::Tracer<MLeaf> : dustman::FieldList<MLeaf> {};

template <>
struct dustman::Tracer<MNode> : dustman::FieldList<MNode, &MNode::leaf> {};

template <>
struct dustman::Tracer<MCyclic> : dustman::FieldList<MCyclic, &MCyclic::next> {};

template <>
struct dustman::Tracer<MCountedLeaf> : dustman::FieldList<MCountedLeaf> {};

template <>
struct dustman::Tracer<MHugeWithRef> : dustman::FieldList<MHugeWithRef, &MHugeWithRef::leaf> {};

TEST_CASE("minor_collect promotes a reachable young object to old", "[minor]") {
  dustman::Root<MLeaf> r {dustman::alloc<MLeaf>()};
  r->v = 42;

  MLeaf* orig = r.get();
  REQUIRE(dustman::detail::header_of(orig)->generation == dustman::detail::Generation::Young);

  dustman::minor_collect();

  MLeaf* moved = r.get();
  REQUIRE(moved != nullptr);
  REQUIRE(moved != orig);
  REQUIRE(dustman::detail::header_of(moved)->generation == dustman::detail::Generation::Old);
  REQUIRE(moved->v == 42);
}

TEST_CASE("minor_collect reclaims unreachable young objects", "[minor]") {
  m_leaf_dtor_count = 0;
  constexpr int N = 32;
  for (int i = 0; i < N; ++i) {
    auto orphan = dustman::alloc<MCountedLeaf>();
    (void)orphan;
  }

  dustman::minor_collect();

  REQUIRE(m_leaf_dtor_count >= N);
}

TEST_CASE("minor_collect preserves a young object reachable via old->young", "[minor]") {
  dustman::Root<MNode> rn {dustman::alloc<MNode>()};
  rn->payload = 100;

  dustman::minor_collect();
  REQUIRE(dustman::detail::header_of(rn.get())->generation == dustman::detail::Generation::Old);

  dustman::gc_ptr<MLeaf> leaf = dustman::alloc<MLeaf>();
  leaf->v = 77;
  MLeaf* leaf_orig = leaf.get();
  REQUIRE(dustman::detail::header_of(leaf_orig)->generation
          == dustman::detail::Generation::Young);

  rn->leaf = leaf;

  dustman::minor_collect();

  MLeaf* leaf_moved = rn->leaf.get();
  REQUIRE(leaf_moved != nullptr);
  REQUIRE(leaf_moved != leaf_orig);
  REQUIRE(dustman::detail::header_of(leaf_moved)->generation
          == dustman::detail::Generation::Old);
  REQUIRE(leaf_moved->v == 77);
}

TEST_CASE("minor_collect preserves cyclic young refs and updates both endpoints", "[minor]") {
  dustman::Root<MCyclic> ra {dustman::alloc<MCyclic>()};
  ra->tag = 1;
  dustman::gc_ptr<MCyclic> b = dustman::alloc<MCyclic>();
  b->tag = 2;

  ra->next = b;
  b->next = ra;

  dustman::minor_collect();

  MCyclic* a_new = ra.get();
  REQUIRE(a_new->tag == 1);
  REQUIRE(dustman::detail::header_of(a_new)->generation == dustman::detail::Generation::Old);

  MCyclic* b_new = a_new->next.get();
  REQUIRE(b_new != nullptr);
  REQUIRE(b_new->tag == 2);
  REQUIRE(dustman::detail::header_of(b_new)->generation == dustman::detail::Generation::Old);

  REQUIRE(b_new->next.get() == a_new);
}

TEST_CASE("minor_collect preserves young reachable only via a huge object", "[minor]") {
  dustman::Root<MHugeWithRef> rh {dustman::alloc<MHugeWithRef>()};
  dustman::gc_ptr<MLeaf> leaf = dustman::alloc<MLeaf>();
  leaf->v = 999;
  MLeaf* leaf_orig = leaf.get();

  rh->leaf = leaf;

  dustman::minor_collect();

  MLeaf* leaf_moved = rh->leaf.get();
  REQUIRE(leaf_moved != nullptr);
  REQUIRE(leaf_moved != leaf_orig);
  REQUIRE(dustman::detail::header_of(leaf_moved)->generation
          == dustman::detail::Generation::Old);
  REQUIRE(leaf_moved->v == 999);
}

TEST_CASE("successive minor_collects preserve data across multiple cycles", "[minor]") {
  dustman::Root<MLeaf> r {dustman::alloc<MLeaf>()};
  r->v = 1;

  for (int i = 0; i < 5; ++i) {
    dustman::minor_collect();
    REQUIRE(r->v == 1);
    REQUIRE(dustman::detail::header_of(r.get())->generation
            == dustman::detail::Generation::Old);
  }
}

TEST_CASE("major collect after minor still works", "[minor]") {
  dustman::Root<MLeaf> r {dustman::alloc<MLeaf>()};
  r->v = 55;

  dustman::minor_collect();
  MLeaf* after_minor = r.get();
  REQUIRE(dustman::detail::header_of(after_minor)->generation
          == dustman::detail::Generation::Old);

  dustman::collect();

  MLeaf* after_major = r.get();
  REQUIRE(after_major != nullptr);
  REQUIRE(after_major->v == 55);
}

TEST_CASE("concurrent minor_collect callers serialise under STW", "[minor][stw]") {
  std::atomic<int> done {0};

  auto worker = [&done] {
    dustman::attach_thread();
    for (int i = 0; i < 20; ++i) {
      dustman::Root<MLeaf> r {dustman::alloc<MLeaf>()};
      r->v = i;
      if ((i & 3) == 0) dustman::minor_collect();
    }
    ++done;
    dustman::detach_thread();
  };

  std::thread t1 {worker};
  std::thread t2 {worker};

  for (int i = 0; i < 20; ++i) {
    dustman::Root<MLeaf> r {dustman::alloc<MLeaf>()};
    r->v = i * 100;
    if ((i & 3) == 0) dustman::minor_collect();
  }

  dustman::enter_native();
  t1.join();
  t2.join();
  dustman::leave_native();

  REQUIRE(done.load() == 2);
}
