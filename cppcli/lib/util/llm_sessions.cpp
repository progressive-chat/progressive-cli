#include "llm_sessions.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace matrixcli { namespace util {

namespace fs = std::filesystem;

std::string llmSessionsDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const fs::path base =
        (xdg && *xdg) ? fs::path(xdg)
                      : (home && *home) ? fs::path(home) / ".local/share"
                                        : fs::path(".");
    return (base / "matrixcli/sessions").string();
}

std::string llmSessionPathFor(const std::string& name) {
    std::string clean;
    clean.reserve(name.size());
    for (unsigned char ch : name) {
        clean += (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
                     ? static_cast<char>(ch)
                     : '_';
    }
    return (fs::path(llmSessionsDir())
            / ("llm-" + (clean.empty() ? "chat" : clean) + ".json")).string();
}

void archiveSession(const std::string& name) {
    const fs::path path = llmSessionPathFor(name);
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    fs::create_directories(llmSessionsDir(), ec);
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string base = name.empty() ? "chat" : name;
    fs::rename(path, fs::path(llmSessionsDir())
                         / ("llm-" + base + "-" + std::to_string(stamp) + ".json"),
               ec);
}

}} // namespace matrixcli::util
