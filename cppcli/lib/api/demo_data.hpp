#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace matrixcli { namespace demo {

using json = nlohmann::json;

struct DemoMember {
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
    std::string membership; // join, invite, leave, ban
    int power_level = 0;
};

struct DemoStateEvent {
    std::string type;      // m.room.member, m.room.topic, m.room.name, etc.
    std::string state_key;
    json content;
    int64_t timestamp = 0;
    std::string sender;
};

struct DemoRoom {
    std::string id;
    std::string name;
    std::string topic;
    std::string avatar_url;
    std::string room_alias;
    bool is_direct = false;
    bool is_encrypted = false;
    std::vector<DemoMember> members;
    std::vector<DemoStateEvent> state_events;
};

struct DemoMessage {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;
    std::string body;
    std::string msgtype;        // m.text, m.notice, m.emote, m.image, m.file, m.video, m.audio
    std::string formatted_body;
    std::string url;            // mxc:// for media
    std::string mimetype;       // for m.file
    int64_t size = 0;          // for m.file
    int64_t width = 0;
    int64_t height = 0;
    std::string relates_to;    // event_id for reactions/edits
    std::string relation_type; // m.annotation, m.replace, m.reference
    bool is_encrypted = false;
    int64_t timestamp = 0;
};

struct DemoDevice {
    std::string device_id;
    std::string display_name;
    std::string last_seen_ip;
    int64_t last_seen_ts;
    bool verified = false;
};

class DemoData {
public:
    static DemoData& instance();

    const std::vector<DemoRoom>& rooms() const { return _rooms; }
    const std::vector<DemoMessage>& allMessages() const { return _messages; }
    const std::vector<DemoDevice>& devices() const { return _devices; }

    DemoRoom* roomById(const std::string& room_id);
    const DemoRoom* roomById(const std::string& room_id) const;

    std::vector<DemoMessage> messagesForRoom(const std::string& room_id,
                                              const std::string& from = "",
                                              const std::string& before = "",
                                              int limit = 20) const;

    void addMessage(const DemoMessage& msg);

    bool save(const std::string& path);
    bool load(const std::string& path);

    // JSON rendering
    static json renderRoomJson(const DemoRoom& r);
    static json renderMessageJson(const DemoMessage& m);
    static json renderMemberJson(const DemoMember& m);
    static json renderStateEventJson(const DemoStateEvent& e);
    static json renderDeviceJson(const DemoDevice& d);
    static json renderEncryptedEventJson(const DemoMessage& m);

    // Text rendering
    static std::string renderRoomText(const DemoRoom& r);
    static std::string renderMessageText(const DemoMessage& m);
    static std::string renderMemberText(const DemoMember& m);
    static std::string renderDeviceText(const DemoDevice& d);

    // Markdown rendering
    static std::string renderRoomMarkdown(const DemoRoom& r);
    static std::string renderMessageMarkdown(const DemoMessage& m);

private:
    DemoData();
    DemoData(const DemoData&) = delete;
    void generate();

    std::vector<DemoRoom> _rooms;
    std::vector<DemoMessage> _messages;
    std::vector<DemoDevice> _devices;
    int64_t _nextId = 0;
    std::string nextEventId(const std::string& prefix = "$evt");
};

}} // namespace matrixcli::demo
