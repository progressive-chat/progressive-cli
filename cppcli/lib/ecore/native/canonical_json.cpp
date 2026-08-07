#include "progressive/canonical_json.hpp"
#include <vector>
#include <algorithm>
#include <set>

namespace progressive {

// ==== Canonical JSON Implementation ====
//
// Original Kotlin (JsonCanonicalizer.kt:35-95):
//   Recursively processes JSON objects, arrays, strings, numbers.
//   Object keys sorted with TreeSet (lexicographic).
//   Strings quoted with JSONObject.quote().
//   For arrays: each element canonicalized individually.

// Helper: JSON-string-escape a string value.
// Original Kotlin: JSONObject.quote(any) for strings
static std::string quoteJsonString(const std::string& s) {
    std::string result = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    result += "\\u";
                    char hex[5];
                    snprintf(hex, sizeof(hex), "%04x", static_cast<unsigned char>(c));
                    result += hex;
                } else {
                    result += c;
                }
        }
    }
    result += "\"";
    return result;
}

// Forward declaration
static std::string canonicalizeRecursive(const std::string& json, size_t& pos);

// Skip whitespace
static void skipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
        || json[pos] == '\n' || json[pos] == '\r')) pos++;
}

// Parse a JSON value starting at pos, return canonical string, advance pos
static std::string parseAndCanonicalize(const std::string& json, size_t& pos) {
    skipWhitespace(json, pos);
    if (pos >= json.size()) return "";

    char c = json[pos];

    if (c == '{') {
        // Object
        pos++; // skip '{'
        std::set<std::string> keys; // TreeSet = sorted
        std::vector<std::pair<std::string, std::string>> entries;

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') { pos++; return "{}"; }

        while (pos < json.size()) {
            skipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] == '}') break;

            // Key must be a string
            if (json[pos] != '"') break;
            pos++; // skip opening quote
            std::string key;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) {
                    pos++;
                    switch (json[pos]) {
                        case '"': key += '"'; break;
                        case '\\': key += '\\'; break;
                        case '/': key += '/'; break;
                        case 'b': key += '\b'; break;
                        case 'f': key += '\f'; break;
                        case 'n': key += '\n'; break;
                        case 'r': key += '\r'; break;
                        case 't': key += '\t'; break;
                        default: key += json[pos];
                    }
                } else {
                    key += json[pos];
                }
                pos++;
            }
            pos++; // skip closing quote

            skipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] != ':') break;
            pos++; // skip ':'

            std::string value = parseAndCanonicalize(json, pos);
            entries.push_back({key, value});
            keys.insert(key);

            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') pos++;
            else break;
        }

        // Sort by key (already sorted in std::set order)
        // Actually we need to re-output in sorted key order
        std::string result = "{";
        bool first = true;
        for (const auto& key : keys) {
            // Find the value for this key
            for (const auto& [k, v] : entries) {
                if (k == key) {
                    if (!first) result += ",";
                    first = false;
                    result += quoteJsonString(key) + ":" + v;
                    break;
                }
            }
        }
        result += "}";

        // Skip past '}' if we haven't already
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') pos++;

        return result;
    }

    if (c == '[') {
        // Array — preserve order, canonicalize each element
        pos++; // skip '['
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') { pos++; return "[]"; }

        std::string result = "[";
        bool first = true;
        while (pos < json.size()) {
            skipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] == ']') break;
            if (!first) result += ",";
            first = false;
            result += parseAndCanonicalize(json, pos);
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') pos++;
        }
        result += "]";

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') pos++;

        return result;
    }

    if (c == '"') {
        // String
        pos++; // skip opening quote
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    case '/': value += '/'; break;
                    case 'b': value += '\b'; break;
                    case 'f': value += '\f'; break;
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    case 'u': {
                        // Skip \uXXXX — just pass through
                        value += "\\u";
                        for (int i = 0; i < 4 && pos + 1 < json.size(); i++) {
                            pos++;
                            value += json[pos];
                        }
                        break;
                    }
                    default:
                        value += json[pos];
                }
            } else {
                value += json[pos];
            }
            pos++;
        }
        pos++; // skip closing quote

        // Original Kotlin: forward slashes are kept as-is, NOT escaped
        // (replace "\\/" → "/" after canonicalization)
        return quoteJsonString(value);
    }

    // Number, boolean, or null
    size_t start = pos;
    if (c == '-' || (c >= '0' && c <= '9')) {
        // Number
        while (pos < json.size() && (json[pos] == '-' || json[pos] == '.'
            || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+'
            || (json[pos] >= '0' && json[pos] <= '9'))) pos++;
    } else {
        // true, false, null
        while (pos < json.size() && json[pos] >= 'a' && json[pos] <= 'z') pos++;
    }
    return json.substr(start, pos - start);
}

std::string canonicalizeJson(const std::string& json) {
    size_t pos = 0;
    std::string result = parseAndCanonicalize(json, pos);

    // Original Kotlin: replace("\\/", "/") — forward slashes must not be escaped
    // Actually in our implementation we already pass them through as-is

    return result;
}

} // namespace progressive
