// src/core/crypto/sas.cpp
#include "sas.hpp"
#include "random.hpp"
#include "../debug_log.hpp"

#include <olm/olm.h>
#include <olm/sas.h>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace progressive::desktop {

static const char B64_C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const uint8_t* data, size_t len) {
    std::string r;
    int val = 0, vb = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + data[i]; vb += 8;
        while (vb >= 0) { r.push_back(B64_C[(val>>vb)&0x3F]); vb -= 6; }
    }
    if (vb > -6) r.push_back(B64_C[((val<<8)>>(vb+8))&0x3F]);
    while (r.size()%4) r.push_back('=');
    return r;
}

static std::vector<uint8_t> b64Decode(const std::string& in) {
    std::vector<uint8_t> r;
    int val = 0, vb = -8;
    for (char c : in) {
        if (c == '=') break;
        const char* p = strchr(B64_C, c); if (!p) continue;
        val = (val<<6)+(int)(p-B64_C); vb += 6;
        if (vb >= 0) { r.push_back((uint8_t)((val>>vb)&0xFF)); vb -= 8; }
    }
    return r;
}

SasSession::~SasSession() {
    if (sas) {
        olm_clear_sas(static_cast<OlmSAS*>(sas));
        free(sas);
        sas = nullptr;
    }
}

SasSession sasCreate() {
    SasSession result;
    size_t size = olm_sas_size();
    void* sasBuf = malloc(size);
    if (!sasBuf) return result;
    auto* sas = olm_sas(sasBuf);

    size_t randLen = olm_create_sas_random_length(sas);
    std::vector<uint8_t> random(randLen);
    fillCryptoRandom(random.data(), randLen);

    size_t ret = olm_create_sas(sas, random.data(), randLen);
    if (ret == olm_error()) {
        LOG(LogChannel::E2EE, "sasCreate: olm_create_sas failed: %s",
            olm_sas_last_error(sas));
        free(sasBuf);
        return result;
    }

    size_t keyLen = olm_sas_pubkey_length(sas);
    std::string pubkey(keyLen, '\0');
    olm_sas_get_pubkey(sas, &pubkey[0], pubkey.size());
    pubkey.resize(keyLen);

    result.sas = sasBuf;
    result.ourPubkey = pubkey;
    result.valid = true;
    return result;
}

bool sasSetTheirKey(SasSession& sas, const std::string& theirPubkeyBase64) {
    if (!sas.valid || !sas.sas) return false;
    auto* olmSas = static_cast<OlmSAS*>(sas.sas);
    // libolm quirk: olm_sas_set_their_key decodes base64 IN-PLACE (sas.c),
    // corrupting the caller's buffer. Always pass a copy.
    std::string copy = theirPubkeyBase64;
    size_t ret = olm_sas_set_their_key(olmSas, copy.data(), copy.size());
    if (ret == olm_error()) {
        LOG(LogChannel::E2EE, "sasSetTheirKey failed: %s", olm_sas_last_error(olmSas));
        return false;
    }
    sas.theirKeySet = true;
    return true;
}

std::string sasGenerateBytes(SasSession& sas, const std::string& info) {
    if (!sas.valid || !sas.sas || !sas.theirKeySet) return {};
    auto* olmSas = static_cast<OlmSAS*>(sas.sas);
    uint8_t bytes[6];
    size_t ret = olm_sas_generate_bytes(olmSas, info.data(), info.size(), bytes, 6);
    if (ret == olm_error()) {
        LOG(LogChannel::E2EE, "sasGenerateBytes failed");
        return {};
    }
    return std::string(reinterpret_cast<char*>(bytes), 6);
}

std::string sasCalculateMac(SasSession& sas, const std::string& message,
                              const std::string& info) {
    if (!sas.valid || !sas.sas || !sas.theirKeySet) return {};
    auto* olmSas = static_cast<OlmSAS*>(sas.sas);
    size_t macLen = olm_sas_mac_length(olmSas);
    std::string mac(macLen, '\0');
    size_t ret = olm_sas_calculate_mac_fixed_base64(olmSas,
        reinterpret_cast<const uint8_t*>(message.data()), message.size(),
        info.data(), info.size(),
        &mac[0], macLen);
    if (ret == olm_error()) return {};
    mac.resize(macLen);
    return mac;
}

bool sasVerifyMac(SasSession& sas, const std::string& theirMacBase64,
                   const std::string& message, const std::string& info) {
    auto ourMac = sasCalculateMac(sas, message, info);
    if (ourMac.empty()) return false;
    return ourMac == theirMacBase64;
}

} // namespace progressive::desktop
