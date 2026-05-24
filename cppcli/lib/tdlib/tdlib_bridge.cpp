#include "tdlib_bridge.hpp"
#include "../util/logger.hpp"

#include <dlfcn.h>
#include <cstring>
#include <chrono>

namespace matrixcli { namespace tdlib {

TdBridge::TdBridge() {}

TdBridge::~TdBridge() {
    shutdown();
}

bool TdBridge::initialize() {
    // Try to load libtdjson
    _handle = dlopen("libtdjson.so", RTLD_NOW);
    if (!_handle) _handle = dlopen("libtdjson.so.1.8", RTLD_NOW);
    if (!_handle) {
        // Try common paths
        const char* paths[] = {
            "/usr/lib/libtdjson.so",
            "/usr/local/lib/libtdjson.so",
            "/opt/tdlib/lib/libtdjson.so",
            nullptr
        };
        for (int i = 0; paths[i]; i++) {
            _handle = dlopen(paths[i], RTLD_NOW);
            if (_handle) break;
        }
    }
    if (!_handle) {
        util::Logger::instance().warn("TDLib not available (libtdjson.so not found)");
        return false;
    }

    _td_create  = (void*(*)())dlsym(_handle, "td_json_client_create");
    _td_send    = (void(*)(void*,const char*))dlsym(_handle, "td_json_client_send");
    _td_receive = (const char*(*)(void*,double))dlsym(_handle, "td_json_client_receive");
    _td_destroy = (void(*)(void*))dlsym(_handle, "td_json_client_destroy");

    if (!_td_create || !_td_send || !_td_receive || !_td_destroy) {
        util::Logger::instance().error("TDLib symbols missing");
        dlclose(_handle);
        _handle = nullptr;
        return false;
    }

    _client = _td_create();
    if (!_client) return false;

    _available = true;
    _running = true;
    _eventThread = std::make_unique<std::thread>(&TdBridge::eventLoop, this);

    util::Logger::instance().info("TDLib bridge initialized");
    return true;
}

void TdBridge::shutdown() {
    _running = false;
    if (_eventThread && _eventThread->joinable()) _eventThread->join();
    if (_client && _td_destroy) _td_destroy(_client);
    if (_handle) dlclose(_handle);
    _client = nullptr; _handle = nullptr; _available = false;
    _authState = TdAuthState::Closed;
}

void TdBridge::send(const json& j) {
    if (!_available || !_client || !_td_send) return;
    std::string s = j.dump();
    _td_send(_client, s.c_str());
}

json TdBridge::receive(double timeout) {
    if (!_available || !_client || !_td_receive) return json::object();
    const char* resp = _td_receive(_client, timeout);
    if (!resp) return json::object();
    try { return json::parse(resp); }
    catch (...) { return json::object(); }
}

void TdBridge::setTdlibParams(int api_id, const std::string& api_hash,
                               const std::string& device_model,
                               const std::string& system_version,
                               const std::string& app_version) {
    send({
        {"@type", "setTdlibParameters"},
        {"api_id", api_id},
        {"api_hash", api_hash},
        {"device_model", device_model},
        {"system_version", system_version},
        {"application_version", app_version},
        {"database_directory", "/tmp/tdlib"},
        {"use_message_database", true},
        {"use_secret_chats", true},
        {"use_test_dc", false}
    });
    _authState = TdAuthState::WaitPhoneNumber;
}

void TdBridge::sendPhoneNumber(const std::string& phone) {
    send({
        {"@type", "setAuthenticationPhoneNumber"},
        {"phone_number", phone}
    });
    _authState = TdAuthState::WaitCode;
}

void TdBridge::sendAuthCode(const std::string& code) {
    send({
        {"@type", "checkAuthenticationCode"},
        {"code", code}
    });
}

void TdBridge::sendPassword(const std::string& password) {
    send({
        {"@type", "checkAuthenticationPassword"},
        {"password", password}
    });
}

std::vector<TdChat> TdBridge::getChats(int limit) {
    std::vector<TdChat> result;
    send({{"@type", "getChats"}, {"limit", limit}});

    // Read response synchronously
    for (int i = 0; i < 10; i++) {
        auto j = receive(1.0);
        if (j.value("@type", "") == "chats") {
            for (auto& chat_id : j.value("chat_ids", json::array())) {
                // Request chat info
                send({{"@type", "getChat"}, {"chat_id", chat_id.get<int64_t>()}});
            }
        }
        if (j.value("@type", "") == "chat") {
            TdChat c;
            c.id = j["id"].get<int64_t>();
            c.title = j.value("title", "Chat " + std::to_string(c.id));
            c.type = j.value("type", json::object()).value("@type", "chatTypePrivate");
            c.unread_count = j.value("unread_count", 0);
            result.push_back(c);
            if ((int)result.size() >= limit) break;
        }
    }
    return result;
}

std::vector<TdMessage> TdBridge::getChatHistory(int64_t chat_id, int64_t from_id, int limit) {
    std::vector<TdMessage> result;
    send({
        {"@type", "getChatHistory"},
        {"chat_id", chat_id},
        {"from_message_id", from_id},
        {"offset", 0},
        {"limit", limit},
        {"only_local", false}
    });

    for (int i = 0; i < 10; i++) {
        auto j = receive(1.0);
        if (j.value("@type", "") == "messages") {
            for (auto& msg : j.value("messages", json::array())) {
                TdMessage m;
                m.id = msg["id"].get<int64_t>();
                m.chat_id = msg["chat_id"].get<int64_t>();
                m.date = msg["date"].get<int64_t>();
                m.is_outgoing = msg.value("is_outgoing", false);
                auto ct = msg.value("content", json::object());
                if (ct.value("@type", "") == "messageText") {
                    m.text = ct.value("text", json::object()).value("text", "");
                } else if (ct.value("@type", "") == "messagePhoto") {
                    m.text = "[Photo] " + ct.value("caption", json::object()).value("text", "");
                } else {
                    m.text = ct.value("@type", "[media]");
                }
                result.push_back(m);
            }
            break;
        }
    }
    return result;
}

void TdBridge::sendMessage(int64_t chat_id, const std::string& text) {
    send({
        {"@type", "sendMessage"},
        {"chat_id", chat_id},
        {"input_message_content", {
            {"@type", "inputMessageText"},
            {"text", {
                {"@type", "formattedText"},
                {"text", text}
            }}
        }}
    });
}

void TdBridge::eventLoop() {
    while (_running.load()) {
        auto update = receive(0.5);
        if (update.empty()) continue;

        std::string type = update.value("@type", "");

        if (type == "updateAuthorizationState") {
            auto& state = update["authorization_state"];
            std::string st = state.value("@type", "");
            if (st == "authorizationStateReady") _authState = TdAuthState::Ready;
            else if (st == "authorizationStateWaitPhoneNumber") _authState = TdAuthState::WaitPhoneNumber;
            else if (st == "authorizationStateWaitCode") _authState = TdAuthState::WaitCode;
            else if (st == "authorizationStateWaitPassword") _authState = TdAuthState::WaitPassword;
            else if (st == "authorizationStateClosed") _authState = TdAuthState::Closed;
            else if (st == "authorizationStateLoggingOut") _authState = TdAuthState::LoggingOut;
        } else if (type == "updateNewMessage") {
            if (_eventCb) _eventCb(update["message"]);
        }

        if (_eventCb && !type.empty() && type.find("update") == 0) {
            _eventCb(update);
        }
    }
}

}} // namespace matrixcli::tdlib
