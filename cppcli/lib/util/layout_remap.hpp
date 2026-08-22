#pragma once

#include <string>
#include <unordered_map>
#include <cctype>

namespace matrixcli { namespace util {

// Remap text typed with the wrong keyboard layout (e.g. a Cyrillic layout when
// the user meant Latin, or vice versa) by swapping each character for the one
// at the same physical key position. The Cyrillic side is stored as numeric
// Unicode code points so the source stays free of non-ASCII literals.
inline std::string keyboardLayoutRemap(const std::string& in) {
    // US-QWERTY keys in physical order, paired with the Cyrillic character
    // produced at the same position (numeric code points, no Cyrillic literal).
    static const char* kLatin = "qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
    static const unsigned kCyr[] = {
        0x0439, 0x0446, 0x0443, 0x043A, 0x0435, 0x043D, 0x0433, 0x0448, 0x0449,
        0x0437, 0x0445, 0x044A, 0x0451, 0x0444, 0x044B, 0x0432, 0x0430, 0x043F,
        0x0440, 0x043E, 0x043B, 0x0434, 0x0436, 0x044D, 0x044F, 0x0447, 0x0441,
        0x043C, 0x0438, 0x0442, 0x044C, 0x0431, 0x044E, 0x002E};
    static std::unordered_map<char32_t, char32_t> L2C, C2L;
    if (L2C.empty()) {
        for (size_t i = 0; i < 34; ++i) {
            char32_t l = (char32_t)(unsigned char)kLatin[i];
            L2C[l] = kCyr[i];
            C2L[kCyr[i]] = l;
        }
    }
    auto cyrUpper = [](char32_t c) -> char32_t {
        if (c >= 0x0430 && c <= 0x044F) return (char32_t)(c - 0x20);
        if (c == 0x0451) return (char32_t)0x0401;
        return c;
    };
    auto cyrLower = [](char32_t c) -> char32_t {
        if (c >= 0x0410 && c <= 0x042F) return (char32_t)(c + 0x20);
        if (c == 0x0401) return (char32_t)0x0451;
        return c;
    };
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char b = (unsigned char)in[i];
        char32_t cp;
        int len;
        if (b < 0x80) {
            cp = b;
            len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = ((char32_t)(b & 0x1F) << 6) | (in[i + 1] & 0x3F);
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = ((char32_t)(b & 0x0F) << 12) | ((char32_t)(in[i + 1] & 0x3F) << 6) |
                 (in[i + 2] & 0x3F);
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = ((char32_t)(b & 0x07) << 18) | ((char32_t)(in[i + 1] & 0x3F) << 12) |
                 ((char32_t)(in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
            len = 4;
        } else {
            cp = b;
            len = 1;
        }
        char32_t o = cp;
        if (cp < 128) {
            char32_t lc = (char32_t)::tolower((int)cp);
            auto it = L2C.find(lc);
            if (it != L2C.end()) {
                o = it->second;
                if (::isupper((int)cp)) o = cyrUpper(o);
            }
        } else if (cp >= 0x0400 && cp <= 0x04FF) {
            char32_t low = cyrLower(cp);
            auto it = C2L.find(low);
            if (it != C2L.end()) {
                o = (char32_t)it->second;
                if (::isupper((int)cp)) o = (char32_t)::toupper((int)o);
            }
        }
        if (o < 0x80)
            out += (char)o;
        else if (o < 0x800) {
            out += (char)(0xC0 | (o >> 6));
            out += (char)(0x80 | (o & 0x3F));
        } else if (o < 0x10000) {
            out += (char)(0xE0 | (o >> 12));
            out += (char)(0x80 | ((o >> 6) & 0x3F));
            out += (char)(0x80 | (o & 0x3F));
        } else {
            out += (char)(0xF0 | (o >> 18));
            out += (char)(0x80 | ((o >> 12) & 0x3F));
            out += (char)(0x80 | ((o >> 6) & 0x3F));
            out += (char)(0x80 | (o & 0x3F));
        }
        i += len;
    }
    return out;
}

} // namespace matrixcli::util
} // namespace matrixcli
