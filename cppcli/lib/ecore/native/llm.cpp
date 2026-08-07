#include "progressive/llm.hpp"
#include "progressive/json_parser.hpp"
#include "progressive/string_utils.hpp"
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace progressive {

// ---- LLM Request Building ----

std::string buildOpenAiRequestBody(const LlmConfig& config, const std::string& prompt) {
    std::ostringstream json;
    json << "{";
    json << R"("model": ")" << config.model << R"(",)";
    json << R"("messages": [)";

    if (!config.systemPrompt.empty()) {
        json << R"({"role": "system", "content": ")" << escapeJson(config.systemPrompt) << R"("},)";
    }
    json << R"({"role": "user", "content": ")" << escapeJson(prompt) << R"("})";

    json << "],";
    json << R"("temperature": )" << config.temperature << ",";
    json << R"("max_tokens": )" << config.maxTokens;
    json << "}";
    return json.str();
}

std::string buildAnthropicRequestBody(const LlmConfig& config, const std::string& prompt) {
    std::ostringstream json;
    json << "{";
    json << R"("model": ")" << config.model << R"(",)";
    json << R"("max_tokens": )" << config.maxTokens << ",";
    json << R"("messages": [)";

    if (!config.systemPrompt.empty()) {
        json << R"({"role": "user", "content": ")" << escapeJson(config.systemPrompt) << R"("},)";
    }
    json << R"({"role": "user", "content": ")" << escapeJson(prompt) << R"("})";

    json << "]";
    json << "}";
    return json.str();
}

std::string buildLlmRequestBody(const LlmConfig& config, const std::string& prompt) {
    switch (config.provider) {
        case LlmProvider::Anthropic: return buildAnthropicRequestBody(config, prompt);
        default: return buildOpenAiRequestBody(config, prompt);
    }
}

std::string buildLlmHeaders(const LlmConfig& config) {
    switch (config.provider) {
        case LlmProvider::Anthropic:
            return "x-api-key: " + config.apiToken + "\n" +
                   "anthropic-version: 2023-06-01\n" +
                   "Content-Type: application/json";
        default:
            return "Authorization: Bearer " + config.apiToken + "\n" +
                   "Content-Type: application/json";
    }
}

// ---- LLM Response Parsing ----

LlmResponse parseOpenAiResponse(const std::string& body, int statusCode) {
    LlmResponse resp;
    resp.statusCode = statusCode;

    if (statusCode != 200) {
        auto err = parseJsonStringValue(body, "error");
        resp.errorMessage = err.empty() ? "Server returned " + std::to_string(statusCode) : err;
        return resp;
    }

    auto choices = parseJsonStringValue(body, "choices");
    if (choices.empty()) {
        resp.errorMessage = "No choices in response";
        return resp;
    }

    auto message = parseJsonStringValue("{" + choices + "}", "message");
    if (message.empty()) {
        resp.errorMessage = "No message in response";
        return resp;
    }

    auto content = parseJsonStringValue("{" + message + "}", "content");
    if (content.empty()) {
        resp.errorMessage = "No content in response";
        return resp;
    }

    resp.success = true;
    resp.text = content;
    return resp;
}

LlmResponse parseAnthropicResponse(const std::string& body, int statusCode) {
    LlmResponse resp;
    resp.statusCode = statusCode;

    if (statusCode != 200) {
        auto err = parseJsonStringValue(body, "error");
        resp.errorMessage = err.empty() ? "Server returned " + std::to_string(statusCode) : err;
        return resp;
    }

    auto content = parseJsonStringValue(body, "content");
    if (content.empty()) {
        // Try "completion" field (legacy API)
        content = parseJsonStringValue(body, "completion");
    }

    if (content.empty()) {
        // Anthropic returns content array: "content": [{"type": "text", "text": "..."}]
        auto contentBlock = parseJsonStringValue(body, "content");
        auto text = parseJsonStringValue("{" + contentBlock + "}", "text");
        if (!text.empty()) content = text;
    }

    if (content.empty()) {
        resp.errorMessage = "No content in response";
        return resp;
    }

    resp.success = true;
    resp.text = content;
    return resp;
}

LlmResponse parseLlmResponse(const std::string& body, int statusCode, LlmProvider provider) {
    switch (provider) {
        case LlmProvider::Anthropic: return parseAnthropicResponse(body, statusCode);
        default: return parseOpenAiResponse(body, statusCode);
    }
}

std::string formatLlmBroadcastMessage(const std::string& prompt, const std::string& response) {
    std::ostringstream out;
    out << "On prompt: \"" << prompt << "\", LLM says: " << response;
    return out.str();
}

// ---- Duplicate Name Detection ----

std::vector<DuplicateNameInfo> detectDuplicateNames(
    const std::vector<DuplicateNameInfo>& users
) {
    auto result = users;

    // Count display names
    std::unordered_map<std::string, int> nameCount;
    for (const auto& u : users) {
        nameCount[u.displayName]++;
    }

    // Mark duplicates
    for (auto& u : result) {
        u.hasDuplicates = nameCount[u.displayName] > 1;
    }

    return result;
}

std::string formatUserDisplayName(const std::string& displayName, const std::string& mxid, bool showMxid) {
    if (!showMxid || mxid.empty()) return displayName;
    return displayName + " (" + mxid + ")";
}

// ---- UserMxidVisibility ----

void UserMxidVisibility::hideMxid(const std::string& mxid) {
    if (std::find(hidden_.begin(), hidden_.end(), mxid) == hidden_.end()) {
        hidden_.push_back(mxid);
    }
}

void UserMxidVisibility::showMxid(const std::string& mxid) {
    hidden_.erase(std::remove(hidden_.begin(), hidden_.end(), mxid), hidden_.end());
}

bool UserMxidVisibility::isVisible(const std::string& mxid) const {
    return std::find(hidden_.begin(), hidden_.end(), mxid) == hidden_.end();
}

void UserMxidVisibility::reset(const std::string& mxid) {
    showMxid(mxid);
}

void UserMxidVisibility::clear() {
    hidden_.clear();
}

std::string UserMxidVisibility::exportJson() const {
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < hidden_.size(); ++i) {
        if (i > 0) json << ",";
        json << R"(")" << hidden_[i] << R"(")";
    }
    json << "]";
    return json.str();
}

void UserMxidVisibility::importJson(const std::string& json) {
    clear();
    size_t pos = 0;
    while (true) {
        pos = json.find('"', pos);
        if (pos == std::string::npos) break;
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) break;
        std::string mxid = json.substr(pos, end - pos);
        if (!mxid.empty() && mxid[0] == '@') hidden_.push_back(mxid);
        pos = end + 1;
    }
}

} // namespace progressive
