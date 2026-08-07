// src/core/crypto/random.cpp
#include "random.hpp"
#include <random>

namespace progressive::desktop {

namespace {
std::function<void(uint8_t*, size_t)>& providerRef() {
    static std::function<void(uint8_t*, size_t)> p;
    return p;
}
void defaultProvider(uint8_t* buf, size_t len) {
    std::random_device rd;
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rd());
}
} // anonymous

void fillCryptoRandom(uint8_t* buf, size_t len) {
    auto& p = providerRef();
    if (p) p(buf, len);
    else defaultProvider(buf, len);
}

void setCryptoRandomProvider(std::function<void(uint8_t*, size_t)> provider) {
    providerRef() = std::move(provider);
}

} // namespace progressive::desktop
