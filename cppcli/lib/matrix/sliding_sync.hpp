#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace matrix {

using json = nlohmann::json;

struct SlidingSyncRoom {
    std::string room_id;
    std::string name;
    std::string avatar_url;
    bool is_dm = false;
    int notification_count = 0;
    int highlight_count = 0;
    int64_t last_event_ts = 0;
    std::vector<json> timeline;
    bool limited = false;
};

struct SlidingSyncList {
    std::string name;
    std::string sort_order = "by_recency";
    std::vector<std::vector<std::string>> required_state;
    int range_start = 0;
    int range_end = 19;
    int timeline_limit = 20;

    json toJson() const;
};

struct SlidingSyncRequest {
    std::vector<SlidingSyncList> lists;
    std::vector<std::string> room_subscriptions;
    std::string pos;
    int timeout_ms = 30000;

    json toJson() const;
};

struct SlidingSyncResponse {
    std::string pos;
    std::vector<SlidingSyncRoom> rooms;
    std::vector<json> to_device;
    std::vector<json> account_data;

    bool fromJson(const std::string& body);
};

// Stateful sliding sync tracker
class SlidingSync {
public:
    void setHomeserver(const std::string& url) { _homeserver = url; }
    void setAccessToken(const std::string& token) { _token = token; }

    void addList(const std::string& name, int range_start = 0, int range_end = 19);
    void subscribeRoom(const std::string& room_id);
    void unsubscribeRoom(const std::string& room_id);

    void scrollDown(int amount);
    void scrollUp(int amount);

    SlidingSyncResponse syncOnce(int timeout_ms = 30000);
    bool isRunning() const { return !_pos.empty(); }

private:
    std::string _homeserver;
    std::string _token;
    std::string _pos;
    std::vector<SlidingSyncList> _lists;
    std::vector<std::string> _subscriptions;
};

}} // namespace matrixcli::matrix
