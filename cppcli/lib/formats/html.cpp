#include "html.hpp"
#include <sstream>
#include <ranges>

namespace matrixcli { namespace formats {

static std::string escapeHTML(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string HTMLRenderer::render(const RenderContext& ctx) const {
    std::ostringstream oss;

    oss << "<!DOCTYPE html><html><head>"
        << "<meta charset=\"utf-8\">"
        << "<title>matrixcli</title>"
        << "<style>body{font-family:monospace;max-width:800px;margin:0 auto;padding:1em;}"
        << ".msg{margin:0.5em 0;padding:0.5em;border-radius:4px;background:#f0f0f0;}"
        << ".sender{font-weight:bold;color:#333;}</style>"
        << "</head><body>";

    if (ctx.data.contains("messages")) {
        for (auto& msg : ctx.data["messages"]) {
            std::string sender = msg.value("sender", "unknown");
            std::string body = msg.value("body", "");
            std::string formatted = msg.value("formatted_body", "");

            if (msg.contains("sender_name")) {
                sender = msg["sender_name"].get<std::string>();
            }

            oss << "<div class=\"msg\">"
                << "<span class=\"sender\">" << escapeHTML(sender) << "</span>: ";

            if (!formatted.empty()) {
                oss << formatted;
            } else {
                for (auto& line : std::views::split(body, '\n')) {
                    std::string l(line.begin(), line.end());
                    while (!l.empty() && l.back() == '\r') l.pop_back();
                    oss << escapeHTML(l) << "<br>";
                }
            }
            oss << "</div>";
        }
    } else if (ctx.data.contains("body")) {
        oss << "<pre>" << escapeHTML(ctx.data["body"].get<std::string>()) << "</pre>";
    } else {
        oss << "<pre>" << escapeHTML(ctx.data.dump(2)) << "</pre>";
    }

    oss << "</body></html>";
    return oss.str();
}

}} // namespace matrixcli::formats
