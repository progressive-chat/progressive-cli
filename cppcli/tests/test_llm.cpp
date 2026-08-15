// The unit tests for the LLM SSE plumbing — every production edge case
// we hit with the real providers is pinned here.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "../lib/util/llm_sse.hpp"

using matrixcli::util::SseEventAssembler;
using matrixcli::util::extractContent;
using nlohmann::json;

static std::vector<std::string> collect(const std::string& input) {
    std::vector<std::string> out;
    SseEventAssembler a;
    a.feed(input, [&](const std::string& p) { out.push_back(p); });
    return out;
}

int main() {
    // 1. The plain single-line events.
    {
        auto evs = collect("data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
        assert(evs.size() == 2);
        assert(evs[0] == "{\"a\":1}");
        assert(evs[1] == "{\"b\":2}");
    }
    // 2. One event spanning several "data:" lines (the joined payload).
    {
        auto evs = collect("data: {\"x\":\ndata: 1}\n\n");
        assert(evs.size() == 1);
        assert(evs[0] == "{\"x\":\n1}");
    }
    // 3. The continuation line without the "data:" prefix.
    {
        auto evs = collect("data: {\"a\":\n\"b\"}\n\n");
        assert(evs.size() == 1);
        assert(evs[0] == "{\"a\":\n\"b\"}");
    }
    // 4. The chunk split mid-line.
    {
        std::vector<std::string> out;
        SseEventAssembler a;
        a.feed("data: {\"a\"", [&](const std::string& p) { out.push_back(p); });
        assert(out.empty());
        a.feed(":1}\n\n", [&](const std::string& p) { out.push_back(p); });
        assert(out.size() == 1);
        assert(out[0] == "{\"a\":1}");
    }
    // 5. The CRLF line endings + the [DONE] skip.
    {
        auto evs = collect("data: {\"a\":1}\r\n\r\ndata: [DONE]\r\n\r\n");
        assert(evs.size() == 1);
        assert(evs[0] == "{\"a\":1}");
    }
    // 6. The "data:" with a space and no space.
    {
        auto evs = collect("data:{\"a\":1}\n\ndata: {\"b\":2}\n\n");
        assert(evs.size() == 2);
        assert(evs[0] == "{\"a\":1}");
    }
    // 7. The event without a trailing blank line (the leftover flush).
    {
        auto evs = collect("data: {\"a\":1}\n");
        assert(evs.size() == 1);
        assert(evs[0] == "{\"a\":1}");
    }

    // ---- extractContent ----
    // 8. The plain string content.
    {
        std::string t, r;
        extractContent(json{{"content", "hello"}}, t, r);
        assert(t == "hello" && r.empty());
    }
    // 9. The null content (the type_error.306 crash source).
    {
        std::string t, r;
        extractContent(json{{"content", nullptr}}, t, r);
        assert(t.empty() && r.empty());
    }
    // 10. The array-of-blocks content with the reasoning blocks.
    {
        std::string t, r;
        extractContent(json{{"content",
                             json::array({{{"type", "reasoning"}, {"text", "think"}},
                                          {{"type", "text"}, {"text", "answer"}}})}},
                       t, r);
        assert(t == "answer");
        assert(r == "think");
    }
    // 11. The reasoning_content string on a delta with no content key.
    {
        std::string t, r;
        extractContent(json{{"reasoning_content", "thinking..."}}, t, r);
        assert(t.empty());
        assert(r == "thinking...");
    }
    // 12. The object with "text".
    {
        std::string t, r;
        extractContent(json{{"content", {{"text", "obj-text"}}}}, t, r);
        assert(t == "obj-text");
    }

    std::printf("test_llm: all ok\n");
    return 0;
}
