#pragma once

// The SSE plumbing for the LLM streams — extracted from the agent so it
// is unit-testable: the providers split events across "data:" lines,
// split chunks mid-line and use \r\n endings, and we fought every one
// of those in production.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace matrixcli { namespace util {

// The SSE event assembler: the chunks may split the lines, one event may
// span several "data:" lines (joined with \n per the spec), a blank line
// ends the event, "[DONE]" is skipped. The complete payloads are
// delivered to the callback in order.
class SseEventAssembler {
public:
    void feed(const std::string& chunk,
              const std::function<void(const std::string& payload)>& onEvent);

private:
    std::string lineBuf_;   // the bytes awaiting the next line split
    std::string eventData_; // the current event's payload lines
};

// The content of a delta/message across the provider shapes: a plain
// string, an array of blocks ({type, text} — the reasoning blocks land
// in `reasoning`), an object with "text", plus the reasoning_content
// field. Null content is ignored (it used to crash .value()).
void extractContent(const nlohmann::json& node, std::string& text,
                    std::string& reasoning);

}} // namespace matrixcli::util
