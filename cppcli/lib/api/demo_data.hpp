#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace matrixcli { namespace demo {

using json = nlohmann::json;

struct DemoRoom {
    std::string id;
    std::string name;
    std::string topic;
    int member_count;
    bool is_direct;
};

struct DemoMessage {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;
    std::string body;
    std::string msgtype;
    std::string formatted_body;
    int64_t timestamp;
};

class DemoData {
public:
    static DemoData& instance();

    const std::vector<DemoRoom>& rooms() const { return _rooms; }
    const std::vector<DemoMessage>& allMessages() const { return _messages; }

    std::vector<DemoMessage> messagesForRoom(const std::string& room_id) const;
    const DemoRoom* roomById(const std::string& room_id) const;

    void addMessage(const DemoMessage& msg);

    json roomsToJson(const std::string& format = "json") const;
    json messagesToJson(const std::string& room_id, int limit = 20,
                        const std::string& format = "json") const;
    json statusToJson() const;
    json syncToJson() const;

    static json renderRoomJson(const DemoRoom& r);
    static json renderMessageJson(const DemoMessage& m);
    static std::string renderRoomText(const DemoRoom& r);
    static std::string renderRoomMarkdown(const DemoRoom& r);
    static std::string renderMessageText(const DemoMessage& m);
    static std::string renderMessageMarkdown(const DemoMessage& m);

private:
    DemoData();
    void generate();

    std::vector<DemoRoom> _rooms;
    std::vector<DemoMessage> _messages;
};

}} // namespace matrixcli::demo
