#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace deltachat {

using json = nlohmann::json;

struct DcChat {
    int id = 0;
    std::string name;
    std::string type; // "single", "group", "verified_group"
    bool is_verified = false;
    int unread_count = 0;
};

struct DcMessage {
    int id = 0;
    int chat_id = 0;
    std::string text;
    int64_t timestamp = 0;
    bool is_outgoing = false;
    std::string sender_name;
};

class DcBridge {
public:
    DcBridge();
    ~DcBridge();

    bool isAvailable() const { return _available; }
    bool isConfigured() const;

    // Lifecycle
    bool initialize(const std::string& db_path = "/tmp/deltachat");
    void shutdown();

    // Config
    void setConfig(const std::string& key, const std::string& value);
    bool configure();

    // Chats
    std::vector<DcChat> getChatList(int flags = 0);
    std::string getChatName(int chat_id);

    // Messages
    std::vector<DcMessage> getChatMessages(int chat_id, int flags = 0);
    int sendMessage(int chat_id, const std::string& text);

    // Event loop
    void startIO();
    void stopIO();
    std::string getNextEvent();

    using EventCallback = std::function<void(const json&)>;
    void setEventCallback(EventCallback cb) { _eventCb = std::move(cb); }

private:
    void eventLoop();

    void* _handle = nullptr;
    void* _ctx = nullptr;
    bool _available = false;
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _eventThread;
    EventCallback _eventCb;

    // Dynamic function pointers (simplified subset)
    void*  (*_dc_create)() = nullptr;
    void   (*_dc_unref)(void*) = nullptr;
    int    (*_dc_open)(void*, const char*, const char*) = nullptr;
    void   (*_dc_close)(void*) = nullptr;
    int    (*_dc_configure)(void*) = nullptr;
    int    (*_dc_is_configured)(void*) = nullptr;
    int    (*_dc_set_config)(void*, const char*, const char*) = nullptr;
    char*  (*_dc_get_config)(void*, const char*) = nullptr;
    int    (*_dc_send_msg)(void*, int, const char*) = nullptr;
    void*  (*_dc_get_chatlist)(void*, int, const char*, int) = nullptr;
    int    (*_dc_chatlist_get_cnt)(void*) = nullptr;
    int    (*_dc_chatlist_get_chat_id)(void*, int) = nullptr;
    void*  (*_dc_get_chat_msgs)(void*, int, int, int*) = nullptr;
    int    (*_dc_array_get_cnt)(void*) = nullptr;
    int    (*_dc_array_get_id)(void*, int) = nullptr;
    void   (*_dc_array_unref)(void*) = nullptr;
    void*  (*_dc_get_msg)(void*, int) = nullptr;
    char*  (*_dc_msg_get_text)(void*) = nullptr;
    int    (*_dc_msg_get_chat_id)(void*) = nullptr;
    int64_t (*_dc_msg_get_timestamp)(void*) = nullptr;
    int    (*_dc_msg_is_outgoing)(void*) = nullptr;
    char*  (*_dc_msg_get_override_sender_name)(void*) = nullptr;
    void   (*_dc_msg_unref)(void*) = nullptr;
    void   (*_dc_delete_msgs)(void*, const int*, int) = nullptr;
    void*  (*_dc_get_chat)(void*, int) = nullptr;
    char*  (*_dc_chat_get_name)(void*) = nullptr;
    int    (*_dc_chat_get_type)(void*) = nullptr;
    void   (*_dc_chat_unref)(void*) = nullptr;
    void   (*_dc_start_io)(void*) = nullptr;
    void   (*_dc_stop_io)(void*) = nullptr;
    char*  (*_dc_get_next_event)(void*) = nullptr;
};

}} // namespace matrixcli::deltachat
