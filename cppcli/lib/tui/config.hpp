#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace tui {

struct TUIConfig {
    bool show_timestamps = false;
    bool show_member_count = true;
    bool compact_mode = false;
    bool mouse_enabled = true;
    bool notification_sound = true;
    std::string date_format = "%H:%M";
    int room_list_width = 25;
    int max_messages = 1000;

    static TUIConfig load(const std::string& path);
    bool save(const std::string& path) const;
};

}} // namespace matrixcli::tui
