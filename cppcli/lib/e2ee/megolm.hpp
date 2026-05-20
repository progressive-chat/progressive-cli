#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace matrixcli { namespace e2ee {

class MegolmOutboundSession {
public:
    MegolmOutboundSession();
    ~MegolmOutboundSession();

    MegolmOutboundSession(const MegolmOutboundSession&) = delete;
    MegolmOutboundSession& operator=(const MegolmOutboundSession&) = delete;
    MegolmOutboundSession(MegolmOutboundSession&& other) noexcept;
    MegolmOutboundSession& operator=(MegolmOutboundSession&& other) noexcept;

    void init();

    std::string encrypt(const std::string& plaintext);

    std::string sessionId();
    uint32_t messageIndex() const;

    std::string sessionKey();

    std::string pickle(const std::string& key);
    static MegolmOutboundSession unpickle(const std::string& key,
                                            const std::string& pickle);

    void* raw() const { return _raw; }

private:
    std::vector<uint8_t> _buffer;
    void* _raw = nullptr;
};

class MegolmInboundSession {
public:
    MegolmInboundSession();
    ~MegolmInboundSession();

    MegolmInboundSession(const MegolmInboundSession&) = delete;
    MegolmInboundSession& operator=(const MegolmInboundSession&) = delete;
    MegolmInboundSession(MegolmInboundSession&& other) noexcept;
    MegolmInboundSession& operator=(MegolmInboundSession&& other) noexcept;

    void init(const std::string& sessionKey);

    void importSession(const std::string& sessionKey);

    std::string decrypt(const std::string& ciphertext, uint32_t& messageIndex);

    std::string sessionId();
    uint32_t firstKnownIndex() const;
    bool isVerified() const;

    std::string exportSession(uint32_t messageIndex);

    std::string pickle(const std::string& key);
    static MegolmInboundSession unpickle(const std::string& key,
                                           const std::string& pickle);

    void* raw() const { return _raw; }

private:
    std::vector<uint8_t> _buffer;
    void* _raw = nullptr;
};

}} // namespace matrixcli::e2ee
