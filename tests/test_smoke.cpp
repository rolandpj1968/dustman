#include <catch2/catch_test_macros.hpp>

#include "dustman/dustman.hpp"

TEST_CASE("dustman version macros are defined", "[smoke]") {
  STATIC_REQUIRE(DUSTMAN_VERSION_MAJOR >= 0);
  STATIC_REQUIRE(DUSTMAN_VERSION_MINOR >= 0);
  STATIC_REQUIRE(DUSTMAN_VERSION_PATCH >= 0);
}

TEST_CASE("dustman namespace constants match macros", "[smoke]") {
  STATIC_REQUIRE(dustman::version_major == DUSTMAN_VERSION_MAJOR);
  STATIC_REQUIRE(dustman::version_minor == DUSTMAN_VERSION_MINOR);
  STATIC_REQUIRE(dustman::version_patch == DUSTMAN_VERSION_PATCH);
}
