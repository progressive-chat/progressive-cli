#include "progressive/room_encryption.hpp"
#include "progressive/json_parser.hpp"
#include <sstream>
#include <chrono>

namespace progressive {

EncryptionConfig parseEncryptionConfig(const std::string& stateContentJson) {
    EncryptionConfig config;
    config.algorithm = parseJsonStringValue(stateContentJson, "algorithm");

    auto rotMs = parseJsonStringValue(stateContentJson, "rotation_period_ms");
    if (!rotMs.empty()) config.rotationPeriodMs = std::stoi(rotMs);

    auto rotMsg = parseJsonStringValue(stateContentJson, "rotation_period_msgs");
    if (!rotMsg.empty()) config.rotationPeriodMessages = std::stoi(rotMsg);

    config.enabled = !config.algorithm.empty();
    config.isDefaultAlgorithm = (config.algorithm == "m.megolm.v1.aes-sha2" ||
                                  config.algorithm.empty());

    // Compute next rotation time if rotation period is set
    if (config.rotationPeriodMs > 0) {
        config.nextRotationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + config.rotationPeriodMs;
    }

    return config;
}

bool isRoomEncrypted(const std::string& stateContentJson) {
    auto algo = parseJsonStringValue(stateContentJson, "algorithm");
    return !algo.empty();
}

std::string getDefaultEncryptionAlgorithm() {
    return "m.megolm.v1.aes-sha2";
}

bool requiresDeviceVerification(const std::string& algorithm) {
    return algorithm == "m.megolm.v1.aes-sha2";
}

EncryptionStatus computeEncryptionStatus(
    const std::string& algorithm,
    const std::vector<bool>& deviceVerified,
    const std::vector<bool>& deviceBlacklisted
) {
    EncryptionStatus status;
    status.algorithm = algorithm;
    status.isEncrypted = !algorithm.empty();
    status.totalDevices = static_cast<int>(deviceVerified.size());

    for (size_t i = 0; i < deviceVerified.size(); ++i) {
        if (i < deviceBlacklisted.size() && deviceBlacklisted[i]) {
            status.blacklistedCount++;
            status.hasBlacklistedDevices = true;
        } else if (deviceVerified[i]) {
            status.verifiedCount++;
        } else {
            status.unverifiedCount++;
            status.hasUnverifiedDevices = true;
        }
    }

    status.isVerified = !status.hasUnverifiedDevices && !status.hasBlacklistedDevices && status.verifiedCount > 0;

    if (status.hasBlacklistedDevices) status.trustLevel = "Warning";
    else if (status.isVerified) status.trustLevel = "Verified";
    else if (status.hasUnverifiedDevices) status.trustLevel = "Warning";
    else status.trustLevel = "Unknown";

    return status;
}

std::string formatEncryptionStatus(const EncryptionStatus& status) {
    std::ostringstream out;
    out << "Encryption: " << (status.isEncrypted ? "Enabled" : "Disabled") << "\n";
    if (status.isEncrypted) {
        out << "Algorithm: " << status.algorithm << "\n";
        out << "Trust: " << status.trustLevel << "\n";
        out << "Devices: " << status.verifiedCount << " verified, "
            << status.unverifiedCount << " unverified";
        if (status.blacklistedCount > 0)
            out << ", " << status.blacklistedCount << " blacklisted";
        out << "\n";
    }
    return out.str();
}

std::string formatEncryptionBadge(const EncryptionStatus& status) {
    if (!status.isEncrypted) return "\xF0\x9F\x94\x93"; // 🔓 open lock
    if (status.isVerified)   return "\xF0\x9F\x94\x92"; // 🔒 locked
    if (status.hasUnverifiedDevices) return "\xE2\x9A\xA0"; // ⚠ warning
    return "\xF0\x9F\x94\x90"; // 🔐 locked with key
}

std::string buildEncryptionContent(const EncryptionConfig& config) {
    std::ostringstream json;
    json << "{";
    json << R"("algorithm": ")" << config.algorithm << R"(")";
    if (config.rotationPeriodMs > 0)
        json << R"(,"rotation_period_ms": )" << config.rotationPeriodMs;
    if (config.rotationPeriodMessages > 0)
        json << R"(,"rotation_period_msgs": )" << config.rotationPeriodMessages;
    json << "}";
    return json.str();
}

bool isRotationDue(const EncryptionConfig& config, int messageCount, int64_t sessionStartMs) {
    if (config.rotationPeriodMessages > 0 && messageCount >= config.rotationPeriodMessages)
        return true;

    if (config.rotationPeriodMs > 0 && sessionStartMs > 0) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - sessionStartMs >= config.rotationPeriodMs) return true;
    }

    return false;
}

} // namespace progressive
