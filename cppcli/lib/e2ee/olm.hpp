#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace matrixcli { namespace e2ee {

class OlmAccount {
public:
    OlmAccount();
    ~OlmAccount();

    OlmAccount(const OlmAccount&) = delete;
    OlmAccount& operator=(const OlmAccount&) = delete;
    OlmAccount(OlmAccount&& other) noexcept;
    OlmAccount& operator=(OlmAccount&& other) noexcept;

    void create();
    std::string identityKeys() const;
    std::string sign(const std::string& message);

    void generateOneTimeKeys(size_t count);
    std::string oneTimeKeys();
    void markKeysPublished();
    size_t maxOneTimeKeys() const;

    void generateFallbackKey();
    std::string fallbackKey() const;
    std::string unpublishedFallbackKey() const;
    void forgetOldFallbackKey();

    std::string pickle(const std::string& key);
    static OlmAccount unpickle(const std::string& key, const std::string& pickle);

    void* raw() const { return _raw; }

private:
    std::vector<uint8_t> _buffer;
    void* _raw = nullptr;
};

class OlmSession {
public:
    OlmSession();
    ~OlmSession();

    OlmSession(const OlmSession&) = delete;
    OlmSession& operator=(const OlmSession&) = delete;
    OlmSession(OlmSession&& other) noexcept;
    OlmSession& operator=(OlmSession&& other) noexcept;

    void createOutbound(OlmAccount& account,
                        const std::string& theirIdentityKey,
                        const std::string& theirOneTimeKey);

    void createInbound(OlmAccount& account,
                       const std::string& oneTimeKeyMessage);

    void createInboundFrom(OlmAccount& account,
                           const std::string& theirIdentityKey,
                           const std::string& oneTimeKeyMessage);

    bool matchesInbound(const std::string& oneTimeKeyMessage);
    bool matchesInboundFrom(const std::string& theirIdentityKey,
                            const std::string& oneTimeKeyMessage);

    std::string sessionId();
    bool hasReceivedMessage() const;

    size_t encryptMessageType();

    std::string encrypt(const std::string& plaintext);

    size_t decryptMaxPlaintextLength(size_t messageType,
                                     const std::string& ciphertext);

    std::string decrypt(size_t messageType, const std::string& ciphertext);

    void removeOneTimeKeys(OlmAccount& account);

    std::string pickle(const std::string& key);
    static OlmSession unpickle(const std::string& key, const std::string& pickle);

    void* raw() const { return _raw; }

private:
    std::vector<uint8_t> _buffer;
    void* _raw = nullptr;
};

}} // namespace matrixcli::e2ee
