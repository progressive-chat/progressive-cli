#include "gemini.hpp"
#include <sstream>
#include <ranges>

namespace matrixcli { namespace formats {

std::string GeminiRenderer::render(const RenderContext& ctx) const {
    std::ostringstream oss;

    if (ctx.data.contains("messages")) {
        for (auto& msg : ctx.data["messages"]) {
            std::string sender = msg.value("sender", "unknown");
            std::string body = msg.value("body", "");

            if (msg.contains("sender_name")) {
                sender = msg["sender_name"].get<std::string>();
            }

            for (auto& line : std::views::split(body, '\n')) {
                std::string l(line.begin(), line.end());
                while (!l.empty() && l.back() == '\r') l.pop_back();
                oss << "> " << l << "\n";
            }
            oss << "-- " << sender << "\n\n";
        }
    } else if (ctx.data.contains("body")) {
        oss << "> " << ctx.data["body"].get<std::string>() << "\n";
    } else {
        oss << "```\n" << ctx.data.dump(2) << "\n```\n";
    }

    return oss.str();
}

}} // namespace matrixcli::formats
