#ifndef PROGRESSIVE_ROOM_ENCRYPTION_HPP
#define PROGRESSIVE_ROOM_ENCRYPTION_HPP

#include <string>
#include <vector>

namespace progressive {

// ---- Room Encryption ----

struct EncryptionConfig {
    std::string algorithm;           // "m.megolm.v1.aes-sha2"
    int rotationPeriodMs = 0;        // 0 = never rotate
    int rotationPeriodMessages = 0;  // 0 = never rotate
    bool enabled = false;

    // Derived
    bool isDefaultAlgorithm = false; // standard megolm
    int64_t nextRotationMs = 0;
};

struct EncryptionStatus {
    bool isEncrypted = false;
    bool isVerified = false;         // all devices verified
    bool hasUnverifiedDevices = false;
    bool hasBlacklistedDevices = false;
    int verifiedCount = 0;
    int unverifiedCount = 0;
    int blacklistedCount = 0;
    int totalDevices = 0;
    std::string algorithm;
    std::string trustLevel;          // "Verified", "Warning", "Unknown"
};

// Parse encryption config from m.room.encryption state event.
EncryptionConfig parseEncryptionConfig(const std::string& stateContentJson);

// Check if a room has encryption enabled.
bool isRoomEncrypted(const std::string& stateContentJson);

// Get the default encryption algorithm.
std::string getDefaultEncryptionAlgorithm();

// Check if an algorithm requires device verification.
bool requiresDeviceVerification(const std::string& algorithm);

// Compute encryption status from device verification states.
EncryptionStatus computeEncryptionStatus(
    const std::string& algorithm,
    const std::vector<bool>& deviceVerified,
    const std::vector<bool>& deviceBlacklisted
);

// Format encryption status as text.
std::string formatEncryptionStatus(const EncryptionStatus& status);

// Format encryption status as a short label for the room header.
std::string formatEncryptionBadge(const EncryptionStatus& status);

// Build the m.room.encryption state event content JSON.
std::string buildEncryptionContent(const EncryptionConfig& config);

// Check if an encryption rotation is due.
bool isRotationDue(const EncryptionConfig& config, int messageCount, int64_t sessionStartMs);

} // namespace progressive

#endif // PROGRESSIVE_ROOM_ENCRYPTION_HPP
