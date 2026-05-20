#include "text.hpp"
#include <sstream>

namespace matrixcli { namespace formats {

std::string TextRenderer::render(const RenderContext& ctx) const {
    std::ostringstream oss;

    if (ctx.data.contains("messages")) {
        for (auto& msg : ctx.data["messages"]) {
            std::string sender = msg.value("sender", "unknown");
            std::string body = msg.value("body", "");

            if (msg.contains("sender_name")) {
                sender = msg["sender_name"].get<std::string>();
            }

            oss << "[" << sender << "] " << body << "\n";
        }
    } else if (ctx.data.contains("body")) {
        oss << ctx.data["body"].get<std::string>() << "\n";
    } else {
        oss << ctx.data.dump(2);
    }

    return oss.str();
}

}} // namespace matrixcli::formats
