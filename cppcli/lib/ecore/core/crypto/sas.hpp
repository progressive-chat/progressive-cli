// src/core/crypto/sas.hpp — OlmSAS wrapper for Short Authentication String verification.
#pragma once
#include <string>
#include <cstdint>
#include <olm/olm.h>
#include <olm/sas.h>

namespace progressive::desktop {

struct SasSession {
    void* sas = nullptr;
    std::string ourPubkey;
    bool theirKeySet = false;
    bool valid = false;

    SasSession() = default;
    ~SasSession();

    SasSession(SasSession&& other) noexcept
        : sas(other.sas), ourPubkey(std::move(other.ourPubkey)),
          theirKeySet(other.theirKeySet), valid(other.valid) {
        other.sas = nullptr;
    }
    SasSession& operator=(SasSession&& other) noexcept {
        if (this != &other) {
            if (sas) { olm_clear_sas(static_cast<OlmSAS*>(sas)); free(sas); }
            sas = other.sas; ourPubkey = std::move(other.ourPubkey);
            theirKeySet = other.theirKeySet; valid = other.valid;
            other.sas = nullptr;
        }
        return *this;
    }
    SasSession(const SasSession&) = delete;
    SasSession& operator=(const SasSession&) = delete;
};

SasSession sasCreate();
bool sasSetTheirKey(SasSession& sas, const std::string& theirPubkeyBase64);
std::string sasGenerateBytes(SasSession& sas, const std::string& info);
std::string sasCalculateMac(SasSession& sas, const std::string& message,
                              const std::string& info);
bool sasVerifyMac(SasSession& sas, const std::string& theirMacBase64,
                   const std::string& message, const std::string& info);

} // namespace progressive::desktop
