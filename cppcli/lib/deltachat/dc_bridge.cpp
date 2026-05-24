#include "dc_bridge.hpp"
#include "../util/logger.hpp"

#include <dlfcn.h>
#include <cstring>

namespace matrixcli { namespace deltachat {

#define LOAD(fn) fn = (decltype(fn))dlsym(_handle, #fn)

DcBridge::DcBridge() {}
DcBridge::~DcBridge() { shutdown(); }

bool DcBridge::initialize(const std::string& db_path) {
    _handle = dlopen("libdeltachat.so", RTLD_NOW);
    if (!_handle) {
        const char* paths[] = {"/usr/lib/libdeltachat.so","/usr/local/lib/libdeltachat.so",nullptr};
        for (int i = 0; paths[i]; i++) if ((_handle = dlopen(paths[i], RTLD_NOW))) break;
    }
    if (!_handle) { util::Logger::instance().warn("DeltaChat not available"); return false; }

    // Load symbols
    LOAD(_dc_create); LOAD(_dc_unref);
    LOAD(_dc_open); LOAD(_dc_close);
    LOAD(_dc_configure); LOAD(_dc_is_configured);
    LOAD(_dc_set_config); LOAD(_dc_get_config);
    LOAD(_dc_send_msg);
    LOAD(_dc_get_chatlist); LOAD(_dc_chatlist_get_cnt); LOAD(_dc_chatlist_get_chat_id);
    LOAD(_dc_get_chat_msgs); LOAD(_dc_array_get_cnt); LOAD(_dc_array_get_id); LOAD(_dc_array_unref);
    LOAD(_dc_get_msg); LOAD(_dc_msg_get_text); LOAD(_dc_msg_get_chat_id);
    LOAD(_dc_msg_get_timestamp); LOAD(_dc_msg_is_outgoing);
    LOAD(_dc_msg_get_override_sender_name); LOAD(_dc_msg_unref);
    LOAD(_dc_delete_msgs);
    LOAD(_dc_get_chat); LOAD(_dc_chat_get_name); LOAD(_dc_chat_get_type); LOAD(_dc_chat_unref);
    LOAD(_dc_start_io); LOAD(_dc_stop_io); LOAD(_dc_get_next_event);

    if (!_dc_create || !_dc_open || !_dc_send_msg) {
        util::Logger::instance().error("DeltaChat core symbols missing");
        dlclose(_handle); _handle = nullptr; return false;
    }

    _ctx = _dc_create();
    if (!_ctx) return false;

    _dc_open(_ctx, db_path.c_str(), nullptr);
    _available = true;

    _running = true;
    _eventThread = std::make_unique<std::thread>(&DcBridge::eventLoop, this);
    util::Logger::instance().info("DeltaChat bridge initialized");
    return true;
}

void DcBridge::shutdown() {
    _running = false;
    if (_eventThread && _eventThread->joinable()) _eventThread->join();
    if (_ctx && _dc_unref) _dc_unref(_ctx);
    if (_handle) dlclose(_handle);
    _ctx = nullptr; _handle = nullptr; _available = false;
}

bool DcBridge::isConfigured() const {
    return _available && _ctx && _dc_is_configured && _dc_is_configured(_ctx) != 0;
}

void DcBridge::setConfig(const std::string& key, const std::string& value) {
    if (_available && _ctx && _dc_set_config)
        _dc_set_config(_ctx, key.c_str(), value.c_str());
}

bool DcBridge::configure() {
    if (_available && _ctx && _dc_configure) return _dc_configure(_ctx) != 0;
    return false;
}

std::vector<DcChat> DcBridge::getChatList(int flags) {
    std::vector<DcChat> result;
    if (!_available) return result;
    auto* list = _dc_get_chatlist(_ctx, flags, nullptr, 0);
    if (!list) return result;
    int cnt = _dc_chatlist_get_cnt(list);
    for (int i = 0; i < cnt; i++) {
        int id = _dc_chatlist_get_chat_id(list, i);
        DcChat c; c.id = id; c.name = getChatName(id);
        auto* chat = _dc_get_chat(_ctx, id);
        if (chat) {
            int type = _dc_chat_get_type(chat);
            c.type = (type == 100 ? "single" : type >= 120 ? "verified_group" : "group");
            c.is_verified = _dc_chat_get_type(chat) >= 120;
            _dc_chat_unref(chat);
        }
        result.push_back(c);
    }
    _dc_array_unref(list);
    return result;
}

std::string DcBridge::getChatName(int chat_id) {
    if (!_available) return "";
    auto* chat = _dc_get_chat(_ctx, chat_id);
    if (!chat) return "";
    char* name = _dc_chat_get_name(chat);
    std::string result = name ? name : "Chat " + std::to_string(chat_id);
    _dc_chat_unref(chat);
    return result;
}

std::vector<DcMessage> DcBridge::getChatMessages(int chat_id, int flags) {
    std::vector<DcMessage> result;
    if (!_available) return result;
    int marker = 0;
    auto* arr = _dc_get_chat_msgs(_ctx, chat_id, flags, &marker);
    if (!arr) return result;
    int cnt = _dc_array_get_cnt(arr);
    for (int i = 0; i < cnt; i++) {
        int id = _dc_array_get_id(arr, i);
        auto* msg = _dc_get_msg(_ctx, id);
        if (msg) {
            DcMessage m; m.id = id; m.chat_id = chat_id;
            char* text = _dc_msg_get_text(msg);
            if (text) m.text = text;
            m.timestamp = _dc_msg_get_timestamp(msg);
            m.is_outgoing = _dc_msg_is_outgoing(msg);
            char* sender = _dc_msg_get_override_sender_name(msg);
            if (sender) m.sender_name = sender;
            _dc_msg_unref(msg);
            result.push_back(m);
        }
    }
    _dc_array_unref(arr);
    return result;
}

int DcBridge::sendMessage(int chat_id, const std::string& text) {
    if (_available && _ctx && _dc_send_msg)
        return _dc_send_msg(_ctx, chat_id, text.c_str());
    return 0;
}

void DcBridge::startIO() { if (_available && _ctx && _dc_start_io) _dc_start_io(_ctx); }
void DcBridge::stopIO() { if (_available && _ctx && _dc_stop_io) _dc_stop_io(_ctx); }

std::string DcBridge::getNextEvent() {
    if (_available && _ctx && _dc_get_next_event) {
        char* evt = _dc_get_next_event(_ctx);
        if (evt) { std::string s(evt); return s; }
    }
    return "{}";
}

void DcBridge::eventLoop() {
    while (_running.load()) {
        std::string evt = getNextEvent();
        if (evt != "{}" && _eventCb) {
            try { _eventCb(json::parse(evt)); } catch (...) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}} // namespace matrixcli::deltachat
