#include "progressive/json_parser.hpp"
#include <cstdlib>
#include <cstddef>

namespace progressive {

std::string parseJsonStringValue(const std::string& json, const std::string& key) {
    // Search for "key"
    std::string search = '"' + key + '"';
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};

    pos += search.size();

    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;

    // Skip whitespace after colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
    if (pos >= json.size()) return {};

    // Handle string value: handles escaped quotes (\") within strings
    if (json[pos] == '"') {
        ++pos;
        auto end = pos;
        while (end < json.size()) {
            if (json[end] == '\\' && end + 1 < json.size()) {
                end += 2; // skip escaped char
                continue;
            }
            if (json[end] == '"') break;
            ++end;
        }
        if (end >= json.size()) return {};
        return json.substr(pos, end - pos);
    }

    // Handle numeric/literal value: 123, true, false, null
    auto end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ' &&
           json[end] != '\t' && json[end] != '\n' && json[end] != '\r') {
        ++end;
    }
    return json.substr(pos, end - pos);
}


bool parseJsonBoolValue(const std::string& json, const std::string& key, bool defaultValue) {
    auto val = parseJsonStringValue(json, key);
    if (val.empty()) return defaultValue;
    return val == "true";
}

int64_t parseJsonInt64Value(const std::string& json, const std::string& key, int64_t defaultValue) {
    auto val = parseJsonStringValue(json, key);
    if (val.empty()) return defaultValue;
    return std::strtoll(val.c_str(), nullptr, 10);
}

} // namespace progressive
