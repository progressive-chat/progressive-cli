#include "llm_sse.hpp"

namespace matrixcli { namespace util {

void SseEventAssembler::feed(
    const std::string& chunk,
    const std::function<void(const std::string& payload)>& onEvent) {
    lineBuf_ += chunk;
    size_t pos;
    while ((pos = lineBuf_.find('\n')) != std::string::npos) {
        std::string line = lineBuf_.substr(0, pos);
        lineBuf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            // The event boundary.
            if (!eventData_.empty() && eventData_ != "[DONE]") {
                if (onEvent) onEvent(eventData_);
            }
            eventData_.clear();
            continue;
        }
        // Any non-blank line belongs to the event (the SSE spec); the
        // "data:" prefix is stripped when present.
        std::string payload = line.rfind("data:", 0) == 0
                                  ? line.substr(5)
                                  : line;
        if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
        if (!eventData_.empty()) eventData_ += "\n";
        eventData_ += payload;
    }
}

void extractContent(const nlohmann::json& node, std::string& text,
                    std::string& reasoning) {
    if (node.contains("content")) {
        const auto& c = node["content"];
        if (c.is_string()) {
            text += c.get<std::string>();
        } else if (c.is_array()) {
            for (const auto& blk : c) {
                if (!blk.is_object()) continue;
                const std::string type = blk.value("type", "");
                const std::string t = blk.value("text", "");
                if (type == "reasoning" || type == "thinking" ||
                    type == "reasoning_content" || type == "analysis")
                    reasoning += t;
                else
                    text += t;
            }
        } else if (c.is_object()) {
            text += c.value("text", "");
        }
    }
    if (node.contains("reasoning_content") &&
        node["reasoning_content"].is_string())
        reasoning += node["reasoning_content"].get<std::string>();
}

}} // namespace matrixcli::util
