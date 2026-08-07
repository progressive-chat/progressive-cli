// src/core/crypto/random.hpp — portable cryptographic random bytes.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

namespace progressive::desktop {

// Fill `buf` with `len` cryptographically-secure random bytes.
// Default impl uses std::random_device (backed by /dev/urandom on
// Linux + Android). Future WASM build calls setCryptoRandomProvider()
// at startup to plug in getentropy/WebCrypto.
void fillCryptoRandom(uint8_t* buf, size_t len);

// Override the default provider. Call once at startup on platforms
// where std::random_device is unsuitable (e.g. emscripten).
// Pass nullptr to restore the default.
void setCryptoRandomProvider(std::function<void(uint8_t*, size_t)> provider);

} // namespace progressive::desktop
