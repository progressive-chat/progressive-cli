#include "config.hpp"
#include <fstream>

namespace matrixcli { namespace tui {

TUIConfig TUIConfig::load(const std::string& path) {
    TUIConfig cfg;
    std::ifstream f(path);
    if (!f) return cfg;

    try {
        auto j = nlohmann::json::parse(f);
        cfg.show_timestamps = j.value("show_timestamps", false);
        cfg.show_member_count = j.value("show_member_count", true);
        cfg.compact_mode = j.value("compact_mode", false);
        cfg.mouse_enabled = j.value("mouse_enabled", true);
        cfg.notification_sound = j.value("notification_sound", true);
        cfg.date_format = j.value("date_format", "%H:%M");
        cfg.room_list_width = j.value("room_list_width", 25);
        cfg.max_messages = j.value("max_messages", 1000);
    } catch (...) {}

    return cfg;
}

bool TUIConfig::save(const std::string& path) const {
    try {
        nlohmann::json j;
        j["show_timestamps"] = show_timestamps;
        j["show_member_count"] = show_member_count;
        j["compact_mode"] = compact_mode;
        j["mouse_enabled"] = mouse_enabled;
        j["notification_sound"] = notification_sound;
        j["date_format"] = date_format;
        j["room_list_width"] = room_list_width;
        j["max_messages"] = max_messages;

        std::ofstream f(path);
        f << j.dump(2);
        return true;
    } catch (...) { return false; }
}

}} // namespace matrixcli::tui
