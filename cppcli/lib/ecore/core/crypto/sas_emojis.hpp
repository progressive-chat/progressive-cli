// src/core/crypto/sas_emojis.hpp — SAS emoji table + computation.
#pragma once
#include <string>
#include <vector>

namespace progressive::desktop {

struct VerificationEmoji {
    std::string emoji;
    std::string description;
};

const std::vector<VerificationEmoji>& sasEmojiTable();
std::vector<VerificationEmoji> computeSasEmojis(const std::string& sasBytes);
std::vector<int> computeSasDecimals(const std::string& sasBytes);
std::string formatSasEmojis(const std::vector<VerificationEmoji>& emojis);
std::string formatSasDecimals(const std::vector<int>& decimals);

} // namespace progressive::desktop
