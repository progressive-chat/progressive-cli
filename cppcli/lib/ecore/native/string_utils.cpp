#include "progressive/string_utils.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace progressive {

std::string sanitizeRoomName(const std::string& input) {
    std::string result;
    for (char c : input) {
        if (c >= 32 && c <= 126) {
            result += c;
        }
    }
    // Collapse multiple spaces
    std::regex multiSpace("  +");
    result = std::regex_replace(result, multiSpace, " ");
    return trim(result);
}

std::string replaceSpaceChars(const std::string& input) {
    std::string result = input;
    // Replace various Unicode space characters with regular space
    std::vector<std::string> spaces = {
        "\u00A0", "\u2000", "\u2001", "\u2002", "\u2003",
        "\u2004", "\u2005", "\u2006", "\u2007", "\u2008",
        "\u2009", "\u200A", "\u202F", "\u205F", "\u3000"
    };
    for (const auto& sp : spaces) {
        size_t pos = 0;
        while ((pos = result.find(sp, pos)) != std::string::npos) {
            result.replace(pos, sp.size(), " ");
            pos++;
        }
    }
    return result;
}

// ==== Count Formatter (from TextUtils.kt:30-45) ====
// Original Kotlin:
//   fun formatCountToShortDecimal(value: Int): String {
//       if (value < 0) return "-" + formatCountToShortDecimal(-value)
//       if (value < 1000) return value.toString()
//       val e = suffixes.floorEntry(value)
//       val divideBy = e?.key
//       val suffix = e?.value
//       val truncated = value / (divideBy!! / 10)
//       val hasDecimal = truncated < 100 && truncated / 10.0 != (truncated / 10).toDouble()
//       return if (hasDecimal) "${truncated / 10.0}$suffix" else "${truncated / 10}$suffix"
//   }

std::string trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && (input[start] == ' ' || input[start] == '\t'
        || input[start] == '\n' || input[start] == '\r')) start++;
    if (start >= input.size()) return "";
    size_t end = input.size() - 1;
    while (end > start && (input[end] == ' ' || input[end] == '\t'
        || input[end] == '\n' || input[end] == '\r')) end--;
    return input.substr(start, end - start + 1);
}

std::string formatCountToShortDecimal(int64_t value) {
    if (value < 0) return "-" + formatCountToShortDecimal(-value);
    if (value < 1000) return std::to_string(value);

    // Original: TreeMap of {1000:"k", 1000000:"M", 1000000000:"G"}
    struct Suffix { int64_t threshold; const char* suffix; };
    static const Suffix suffixes[] = {
        {1000000000, "G"},
        {1000000, "M"},
        {1000, "k"},
    };

    for (const auto& s : suffixes) {
        if (value >= s.threshold) {
            // Original: val truncated = value / (divideBy / 10)
            int64_t divideBy = s.threshold;
            int64_t truncated = value / (divideBy / 10);

            // Original: hasDecimal = truncated < 100 && truncated / 10.0 != (truncated / 10)
            bool hasDecimal = (truncated < 100) && ((truncated % 10) != 0);

            if (hasDecimal) {
                // Show one decimal: "1.2k", "3.4M"
                return std::to_string(truncated / 10) + "." + std::to_string(truncated % 10) + s.suffix;
            } else {
                // No decimal: "12k", "42M"
                return std::to_string(truncated / 10) + s.suffix;
            }
        }
    }

    return std::to_string(value);
}


int computeStringDistance(const std::string& a, const std::string& b, StringDistance algorithm) {
    if (algorithm == StringDistance::LEVENSHTEIN) {
        std::vector<std::vector<int>> dp(a.size()+1, std::vector<int>(b.size()+1));
        for (size_t i=0; i<=a.size(); i++) dp[i][0]=(int)i;
        for (size_t j=0; j<=b.size(); j++) dp[0][j]=(int)j;
        for (size_t i=1; i<=a.size(); i++)
            for (size_t j=1; j<=b.size(); j++)
                dp[i][j]=std::min({dp[i-1][j]+1,dp[i][j-1]+1,dp[i-1][j-1]+(a[i-1]!=b[j-1])});
        return dp[a.size()][b.size()];
    }
    return 0;
}
double computeStringSimilarity(const std::string& a, const std::string& b, StringDistance algorithm) {
    int dist = computeStringDistance(a, b, algorithm);
    int maxLen = (int)std::max(a.size(), b.size());
    return maxLen > 0 ? 1.0 - (double)dist / maxLen : 1.0;
}
std::string findClosestMatch(const std::string& target, const std::vector<std::string>& candidates, StringDistance algorithm) {
    std::string best; int bestDist = INT_MAX;
    for (const auto& c : candidates) {
        int d = computeStringDistance(target, c, algorithm);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    return best;
}
std::string truncateString(const std::string& input, int maxLen, StringTruncation mode, const std::string& ellipsis) {
    if ((int)input.size() <= maxLen) return input;
    int keep = maxLen - (int)ellipsis.size();
    if (keep < 0) keep = 0;
    if (mode == StringTruncation::END) return input.substr(0, keep) + ellipsis;
    if (mode == StringTruncation::START) return ellipsis + input.substr(input.size()-keep);
    int half = keep / 2;
    return input.substr(0, half) + ellipsis + input.substr(input.size()-(keep-half));
}
std::vector<StringSearchResult> findAllOccurrences(const std::string& haystack, const std::string& needle) {
    std::vector<StringSearchResult> r; size_t p = 0;
    while ((p=haystack.find(needle,p)) != std::string::npos) {
        StringSearchResult sr; sr.found=true; sr.position=(int)p; sr.length=(int)needle.size(); r.push_back(sr);
        p += needle.size();
    }
    return r;
}

std::string escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

std::string unescapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            switch (input[i + 1]) {
                case '"':  out += '"';  ++i; break;
                case '\\': out += '\\'; ++i; break;
                case 'n':  out += '\n'; ++i; break;
                case 'r':  out += '\r'; ++i; break;
                case 't':  out += '\t'; ++i; break;
                default:   out += input[i];
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

// ---- IDN / Punycode Support ----

// RFC 3492 Bootstring parameters for Punycode
static const int PUNYCODE_BASE = 36;
static const int PUNYCODE_TMIN = 1;
static const int PUNYCODE_TMAX = 26;
static const int PUNYCODE_SKEW = 38;
static const int PUNYCODE_DAMP = 700;
static const int PUNYCODE_INITIAL_BIAS = 72;
static const int PUNYCODE_INITIAL_N = 128;

static int punycodeAdapt(int delta, int numPoints, bool firstTime) {
    delta = firstTime ? delta / PUNYCODE_DAMP : delta >> 1;
    delta += delta / numPoints;
    int k = 0;
    while (delta > ((PUNYCODE_BASE - PUNYCODE_TMIN) * PUNYCODE_TMAX) / 2) {
        delta /= PUNYCODE_BASE - PUNYCODE_TMIN;
        k += PUNYCODE_BASE;
    }
    return k + (((PUNYCODE_BASE - PUNYCODE_TMIN + 1) * delta) / (delta + PUNYCODE_SKEW));
}

static char punycodeEncodeDigit(int d) {
    if (d < 26) return static_cast<char>('a' + d);
    return static_cast<char>('0' + d - 26);
}

std::string toPunycode(const std::string& domain) {
    if (domain.empty()) return "";
    
    std::string result;
    // Process each label (subdomain) separately
    size_t pos = 0;
    while (pos <= domain.size()) {
        size_t dot = domain.find('.', pos);
        if (dot == std::string::npos) dot = domain.size();
        std::string label = domain.substr(pos, dot - pos);
        
        // Check if label is all ASCII
        bool allAscii = true;
        for (unsigned char c : label) {
            if (c > 127) { allAscii = false; break; }
        }
        
        if (allAscii) {
            if (!result.empty()) result += '.';
            result += label;
        } else {
            // Need Punycode encoding
            // Extract basic (ASCII) code points
            std::vector<int> codepoints;
            std::string basic;
            for (size_t i = 0; i < label.size();) {
                unsigned char c = label[i];
                int cp;
                if (c < 0x80) { cp = c; i++; }
                else if (c < 0xE0) { cp = ((c & 0x1F) << 6) | (label[i+1] & 0x3F); i += 2; }
                else if (c < 0xF0) { cp = ((c & 0x0F) << 12) | ((label[i+1] & 0x3F) << 6) | (label[i+2] & 0x3F); i += 2; }
                else { cp = ((c & 0x07) << 18) | ((label[i+1] & 0x3F) << 12) | ((label[i+2] & 0x3F) << 6) | (label[i+3] & 0x3F); i += 3; }
                
                if (cp < 128) basic += static_cast<char>(cp);
                codepoints.push_back(cp);
            }
            
            std::string encoded = "xn--" + basic;
            if (basic.size() < label.size()) {
                // Non-ASCII code points to encode
                std::vector<int> nonAscii;
                for (int cp : codepoints) if (cp >= 128) nonAscii.push_back(cp);
                std::sort(nonAscii.begin(), nonAscii.end());
                
                int n = PUNYCODE_INITIAL_N;
                int delta = 0;
                int bias = PUNYCODE_INITIAL_BIAS;
                int h = basic.size();
                int b = basic.size();
                
                for (size_t j = 0; j < nonAscii.size();) {
                    int m = nonAscii[j];
                    for (size_t k = j + 1; k < nonAscii.size(); k++) {
                        if (nonAscii[k] < m) m = nonAscii[k];
                    }
                    
                    delta += (m - n) * (h + 1);
                    n = m;
                    
                    for (size_t k = 0; k < nonAscii.size(); k++) {
                        if (nonAscii[k] < n) { delta++; continue; }
                        if (nonAscii[k] > n) break;
                        
                        int q = delta;
                        for (int kk = PUNYCODE_BASE; ; kk += PUNYCODE_BASE) {
                            int t = kk <= bias ? PUNYCODE_TMIN :
                                    kk >= bias + PUNYCODE_TMAX ? PUNYCODE_TMAX : kk - bias;
                            if (q < t) break;
                            encoded += punycodeEncodeDigit(t + ((q - t) % (PUNYCODE_BASE - t)));
                            q = (q - t) / (PUNYCODE_BASE - t);
                        }
                        encoded += punycodeEncodeDigit(q);
                        bias = punycodeAdapt(delta, h + 1, h == b);
                        delta = 0;
                        h++;
                        j++;
                    }
                    delta++;
                    n++;
                }
            }
            
            if (!result.empty()) result += '.';
            result += encoded;
        }
        pos = dot + 1;
    }
    return result;
}

std::string fromPunycode(const std::string& domain) {
    // For now, detect xn-- prefixes and return the original
    // A full Punycode decoder is complex; we handle the display case
    // by checking if the domain needs decoding
    if (domain.empty()) return "";
    
    bool hasPunycode = false;
    std::string result;
    size_t pos = 0;
    while (pos <= domain.size()) {
        size_t dot = domain.find('.', pos);
        if (dot == std::string::npos) dot = domain.size();
        std::string label = domain.substr(pos, dot - pos);
        
        if (label.size() > 4 && label.substr(0, 4) == "xn--") {
            // This is Punycode — attempt to decode
            std::string ascii;
            for (char c : label.substr(4)) {
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ascii += c;
                else if (c == '-') ascii += c;
            }
            // Simplified: return with marker that this could be decoded
            if (!result.empty()) result += '.';
            result += "(xn--" + ascii + ")";  // Placeholder until full decoder
            hasPunycode = true;
        } else {
            if (!result.empty()) result += '.';
            result += label;
        }
        pos = dot + 1;
    }
    
    // If no Punycode found, return as-is (already Unicode)
    return hasPunycode ? result : domain;
}


// ---- URL Utils (ported from UrlUtils.kt) ----

std::string ensureProtocol(const std::string& url) {
    if (url.empty()) return url;
    if (url.find("http") == 0) return url;
    return "https://" + url;
}

std::string ensureTrailingSlash(const std::string& url) {
    if (url.empty()) return url;
    if (url.back() == '/') return url;
    return url + "/";
}

std::string stripHtmlTags(const std::string& html) {
    std::string result;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; continue; }
        if (inTag) { if (c == '>') inTag = false; continue; }
        result += c;
    }
    return result;
}
} // namespace progressive
