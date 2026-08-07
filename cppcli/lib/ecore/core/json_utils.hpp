// src/core/json_utils.hpp — JSON string unescape utility.
#pragma once
#include <string>
#include <string_view>

namespace progressive::desktop {

std::string jsonUnescape(std::string_view s);

} // namespace progressive::desktop
