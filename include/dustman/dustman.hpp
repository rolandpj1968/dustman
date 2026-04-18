#pragma once

// Dustman — a precise garbage collector library for C++.
// See https://github.com/rolandpj1968/dustman for documentation and design.

#define DUSTMAN_VERSION_MAJOR 0
#define DUSTMAN_VERSION_MINOR 0
#define DUSTMAN_VERSION_PATCH 1

namespace dustman {

inline constexpr int version_major = DUSTMAN_VERSION_MAJOR;
inline constexpr int version_minor = DUSTMAN_VERSION_MINOR;
inline constexpr int version_patch = DUSTMAN_VERSION_PATCH;

// Public API will be introduced in subsequent commits.

}  // namespace dustman
