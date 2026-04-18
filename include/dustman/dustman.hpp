#pragma once

#define DUSTMAN_VERSION_MAJOR 0
#define DUSTMAN_VERSION_MINOR 0
#define DUSTMAN_VERSION_PATCH 1

#include "dustman/alloc.hpp"
#include "dustman/gc_ptr.hpp"
#include "dustman/root.hpp"
#include "dustman/tracer.hpp"
#include "dustman/type_info.hpp"
#include "dustman/visitor.hpp"

namespace dustman {

inline constexpr int version_major = DUSTMAN_VERSION_MAJOR;
inline constexpr int version_minor = DUSTMAN_VERSION_MINOR;
inline constexpr int version_patch = DUSTMAN_VERSION_PATCH;

} // namespace dustman
