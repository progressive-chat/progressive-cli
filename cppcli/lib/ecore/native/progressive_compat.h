// third_party/android_shim/progressive_compat.h
//
// Force-included via -include to fix missing STL includes in progressive_native
// modules. The Android NDK's <string>, <vector>, etc. transitively include
// <algorithm>, <cctype>, <numeric>, etc. Strict gcc/clang (e.g. gcc 16 on
// DanctNIX Linux) does NOT — so modules using std::remove/std::find/std::sort
// without an explicit #include <algorithm> fail to compile.
//
// Kept intentionally MINIMAL — only headers that are actually missing on
// desktop but transitively present via Android NDK's <string>/<vector>.
// Heavy headers like <regex>, <random> are NOT here because:
//   (a) real modules that use them already #include them explicitly
//   (b) force-including them on 595 TUs slows the build significantly
//
// This is a build-time compatibility shim, NOT part of the API.
#pragma once

#include <algorithm>      // std::remove, std::find, std::sort, std::transform, std::min/max
#include <cctype>         // std::tolower, std::isspace, std::isalpha
#include <chrono>         // std::chrono::system_clock (used by common_utils.hpp)
#include <climits>        // INT_MAX, INT_MIN, LLONG_MAX (string_utils, timeline_chunk)
#include <cmath>          // std::fmod, std::isnan, std::round, std::floor, std::ceil (displayname_utils)
#include <cstdint>
#include <cstdlib>        // std::strtol, std::strtod
#include <cstring>        // std::strlen, std::memcpy
#include <ctime>          // std::time, std::localtime
#include <functional>     // std::function, std::hash
#include <map>
#include <memory>         // std::shared_ptr, std::unique_ptr
#include <numeric>        // std::accumulate, std::iota
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>        // std::pair, std::move, std::forward
#include <vector>
