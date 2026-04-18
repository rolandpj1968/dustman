#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"

namespace {

struct Marker {
  int v = 0;
};

} // namespace

template <>
struct dustman::Tracer<Marker> : dustman::FieldList<Marker> {};

TEST_CASE("forwarding round-trip via raw buffer", "[forwarding]") {
  alignas(void*) std::byte buffer[64] {};
  void* body = buffer + sizeof(void*);

  const dustman::TypeInfo* ti = &dustman::TypeInfoFor<Marker>::value;
  *reinterpret_cast<const dustman::TypeInfo**>(buffer) = ti;

  REQUIRE_FALSE(dustman::detail::is_forwarded(body));

  alignas(void*) std::byte new_buffer[64] {};
  void* new_body = new_buffer + sizeof(void*);

  dustman::detail::set_forwarded(body, new_body);

  REQUIRE(dustman::detail::is_forwarded(body));
  REQUIRE(dustman::detail::forwarded_to(body) == new_body);
}

TEST_CASE("decode_header reports Normal for a plain TypeInfo header", "[forwarding]") {
  alignas(void*) std::byte buffer[64] {};
  void* body = buffer + sizeof(void*);

  const dustman::TypeInfo* ti = &dustman::TypeInfoFor<Marker>::value;
  *reinterpret_cast<const dustman::TypeInfo**>(buffer) = ti;

  auto view = dustman::detail::decode_header(body);
  REQUIRE(view.kind == dustman::detail::HeaderKind::Normal);
  REQUIRE(view.type == ti);
}

TEST_CASE("decode_header reports Forwarded after set_forwarded", "[forwarding]") {
  alignas(void*) std::byte buffer[64] {};
  void* body = buffer + sizeof(void*);

  alignas(void*) std::byte new_buffer[64] {};
  void* new_body = new_buffer + sizeof(void*);

  dustman::detail::set_forwarded(body, new_body);

  auto view = dustman::detail::decode_header(body);
  REQUIRE(view.kind == dustman::detail::HeaderKind::Forwarded);
  REQUIRE(view.new_body == new_body);
}

TEST_CASE("forwarding preserves the low three bits of the new body address being zero",
          "[forwarding]") {
  alignas(void*) std::byte buffer[64] {};
  void* body = buffer + sizeof(void*);

  alignas(void*) std::byte new_buffer[64] {};
  void* new_body = new_buffer + sizeof(void*);

  REQUIRE((reinterpret_cast<std::uintptr_t>(new_body) & dustman::detail::forwarded_bit) == 0);

  dustman::detail::set_forwarded(body, new_body);
  REQUIRE(dustman::detail::forwarded_to(body) == new_body);
}
