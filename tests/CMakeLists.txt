include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.14.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(dustman_tests
  test_smoke.cpp
  test_alloc.cpp
  test_alloc_stress.cpp
  test_block.cpp
  test_collect.cpp
  test_evacuate.cpp
  test_forwarding.cpp
  test_gc_ptr.cpp
  test_huge.cpp
  test_isolation.cpp
  test_medium.cpp
  test_recycle.cpp
  test_root.cpp
  test_sweep.cpp
  test_tracer.cpp
  test_type_info.cpp
)

target_link_libraries(dustman_tests PRIVATE
  dustman
  Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(dustman_tests)
