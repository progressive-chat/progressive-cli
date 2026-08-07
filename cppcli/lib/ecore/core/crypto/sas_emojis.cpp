// src/core/crypto/sas_emojis.cpp
#include "sas_emojis.hpp"
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace progressive::desktop {

const std::vector<VerificationEmoji>& sasEmojiTable() {
    static const std::vector<VerificationEmoji> table = {
        {"🐶", "Dog"}, {"🐱", "Cat"}, {"🦁", "Lion"}, {"🐎", "Horse"},
        {"🦄", "Unicorn"}, {"🐷", "Pig"}, {"🐘", "Elephant"}, {"🐰", "Rabbit"},
        {"🐼", "Panda"}, {"🐓", "Rooster"}, {"🐧", "Penguin"}, {"🐢", "Turtle"},
        {"🐙", "Octopus"}, {"🐳", "Whale"}, {"🦋", "Butterfly"}, {"🌻", "Sunflower"},
        {"🌴", "Palm Tree"}, {"🌵", "Cactus"}, {"🍇", "Grapes"}, {"🍉", "Watermelon"},
        {"🍋", "Lemon"}, {"🍌", "Banana"}, {"🍍", "Pineapple"}, {"🍎", "Red Apple"},
        {"🍒", "Cherries"}, {"🍓", "Strawberry"}, {"🌽", "Corn"}, {"🍕", "Pizza"},
        {"🎂", "Birthday Cake"}, {"🏆", "Trophy"}, {"🎓", "Graduation Cap"},
        {"🎸", "Guitar"}, {"🎺", "Trumpet"}, {"🔔", "Bell"}, {"🎵", "Musical Note"},
        {"🎄", "Christmas Tree"}, {"🎃", "Pumpkin"}, {"🌎", "Earth"}, {"🌙", "Moon"},
        {"☀️", "Sun"}, {"⭐", "Star"}, {"⚡", "Lightning"}, {"🔥", "Fire"},
        {"🌈", "Rainbow"}, {"❄️", "Snowflake"}, {"💧", "Droplet"}, {"🎈", "Balloon"},
        {"🔑", "Key"}, {"🔒", "Lock"}, {"✏️", "Pencil"}, {"📌", "Pin"},
        {"⌚", "Watch"}, {"📷", "Camera"}, {"🔋", "Battery"}, {"💡", "Light Bulb"},
        {"🏁", "Checkered Flag"}, {"🚀", "Rocket"}, {"🚲", "Bicycle"}, {"🚗", "Car"},
        {"⛵", "Sailboat"}, {"✈️", "Airplane"}, {"🚂", "Train"}, {"🚦", "Traffic Light"},
        {"🔨", "Hammer"}
    };
    return table;
}

std::vector<VerificationEmoji> computeSasEmojis(const std::string& sasBytes) {
    std::vector<VerificationEmoji> result;
    auto& allEmojis = sasEmojiTable();
    if (sasBytes.size() < 6) return result;

    unsigned char B0 = sasBytes[0], B1 = sasBytes[1], B2 = sasBytes[2];
    unsigned char B3 = sasBytes[3], B4 = sasBytes[4], B5 = sasBytes[5];

    int e0 = (B0 >> 2) & 0x3F;
    int e1 = ((B0 & 0x3) << 4) | (B1 >> 4);
    int e2 = ((B1 & 0xF) << 2) | (B2 >> 6);
    int e3 = B2 & 0x3F;
    int e4 = (B3 >> 2) & 0x3F;
    int e5 = ((B3 & 0x3) << 4) | (B4 >> 4);
    int e6 = ((B4 & 0xF) << 2) | (B5 >> 6);

    int indices[] = {e0, e1, e2, e3, e4, e5, e6};
    for (int idx : indices) {
        if (idx >= 0 && idx < (int)allEmojis.size())
            result.push_back(allEmojis[idx]);
    }
    return result;
}

std::vector<int> computeSasDecimals(const std::string& sasBytes) {
    std::vector<int> decimals;
    if (sasBytes.size() < 5) return decimals;

    unsigned char B0 = sasBytes[0], B1 = sasBytes[1], B2 = sasBytes[2];
    unsigned char B3 = sasBytes[3], B4 = sasBytes[4];

    int first  = (B0 << 5 | B1 >> 3) + 1000;
    int second = ((B1 & 0x7) << 10 | B2 << 2 | B3 >> 6) + 1000;
    int third  = ((B3 & 0x3F) << 7 | B4 >> 1) + 1000;

    decimals.push_back(first);
    decimals.push_back(second);
    decimals.push_back(third);
    return decimals;
}

std::string formatSasDecimals(const std::vector<int>& decimals) {
    std::ostringstream out;
    for (size_t i = 0; i < decimals.size(); ++i) {
        if (i > 0) out << " - ";
        out << std::setfill('0') << std::setw(4) << decimals[i];
    }
    return out.str();
}

std::string formatSasEmojis(const std::vector<VerificationEmoji>& emojis) {
    std::ostringstream out;
    for (size_t i = 0; i < emojis.size(); ++i) {
        if (i > 0) out << "  ";
        out << emojis[i].emoji;
    }
    return out.str();
}

} // namespace progressive::desktop
