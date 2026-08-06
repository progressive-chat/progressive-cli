#include "markdown.hpp"
#include <sstream>

namespace matrixcli { namespace formats {

std::string MarkdownRenderer::render(const RenderContext& ctx) const {
    std::ostringstream oss;

    if (ctx.data.contains("messages")) {
        for (auto& msg : ctx.data["messages"]) {
            std::string sender = msg.value("sender", "unknown");
            std::string body = msg.value("body", "");
            std::string formatted = msg.value("formatted_body", "");

            if (msg.contains("sender_name")) {
                sender = msg["sender_name"].get<std::string>();
            }

            oss << "**" << sender << "**: ";

            if (!formatted.empty()) {
                oss << formatted;
            } else {
                oss << body;
            }
            oss << "\n\n";
        }
    } else if (ctx.data.contains("body")) {
        oss << ctx.data["body"].get<std::string>() << "\n";
    } else {
        oss << "```json\n" << ctx.data.dump(2) << "\n```\n";
    }

    return oss.str();
}

}} // namespace matrixcli::formats
