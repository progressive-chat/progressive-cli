#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace tdlib {

using json = nlohmann::json;

// TDLib authorization states
enum class TdAuthState {
    Closed,              // TDLib instance closed
    WaitTdlibParams,     // Need to send setTdlibParameters
    WaitPhoneNumber,     // Need to send phone number
    WaitCode,            // Need to send auth code
    WaitPassword,        // Need to send 2FA password
    Ready,               // Authorized
    LoggingOut,          // Logging out
    Error               // Authorization error
};

struct TdChat {
    int64_t id = 0;
    std::string title;
    std::string type; // "private", "group", "supergroup", "channel"
    int unread_count = 0;
    int64_t last_message_id = 0;
};

struct TdMessage {
    int64_t id = 0;
    int64_t chat_id = 0;
    std::string sender_name;
    std::string text;
    int64_t date = 0;
    bool is_outgoing = false;
};

class TdBridge {
public:
    TdBridge();
    ~TdBridge();

    TdBridge(const TdBridge&) = delete;
    TdBridge& operator=(const TdBridge&) = delete;

    bool isAvailable() const { return _available; }
    TdAuthState authState() const { return _authState.load(); }

    // Lifecycle
    bool initialize();
    void shutdown();

    // Auth
    void setTdlibParams(int api_id, const std::string& api_hash,
                         const std::string& device_model = "matrixcli",
                         const std::string& system_version = "1.0",
                         const std::string& app_version = "1.0");
    void sendPhoneNumber(const std::string& phone);
    void sendAuthCode(const std::string& code);
    void sendPassword(const std::string& password);

    // Chats
    std::vector<TdChat> getChats(int limit = 50);

    // Messages
    std::vector<TdMessage> getChatHistory(int64_t chat_id, int64_t from_id = 0, int limit = 20);
    void sendMessage(int64_t chat_id, const std::string& text);

    // Event loop
    using EventCallback = std::function<void(const json&)>;
    void setEventCallback(EventCallback cb) { _eventCb = std::move(cb); }

private:
    void send(const json& j);
    json receive(double timeout = 0.1);
    void eventLoop();
    void handleUpdate(const json& update);

    void* _handle = nullptr;
    void* _client = nullptr;
    bool _available = false;
    std::atomic<TdAuthState> _authState{TdAuthState::Closed};
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _eventThread;
    EventCallback _eventCb;

    // Dynamic function pointers
    void* (*_td_create)() = nullptr;
    void  (*_td_send)(void*, const char*) = nullptr;
    const char* (*_td_receive)(void*, double) = nullptr;
    void  (*_td_destroy)(void*) = nullptr;
};

}} // namespace matrixcli::tdlib
