#pragma once
#include <string>
#include <random>
#include <cstdio>

namespace progressive::desktop {

inline std::string generateUUID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    char buf[65];
    std::snprintf(buf, sizeof(buf), "%016lx%016lx%016lx%016lx",
                  dis(gen), dis(gen), dis(gen), dis(gen));
    return std::string("pd-") + buf;
}

} // namespace progressive::desktop
