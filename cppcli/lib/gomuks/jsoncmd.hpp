#pragma once
// The gomuks jsoncmd envelope, mirrored from
// go.mau.fi/gomuks/pkg/hicli/jsoncmd/commands.go (pinned: v0.2608.x,
// the go-legacy snapshot in this repo):
//
//   request:  {"command": "<name>", "request_id": N, "data": {...}}
//   success:  {"command": "response", "request_id": N, "data": {...}}
//   failure:  {"command": "error",    "request_id": N, "data": "msg"}
//   event:    {"command": "<event>",  "request_id": 0, "data": {...}}
//
// Events the M1 surface cares about: run_id, client_state, sync_status,
// sync_complete, send_complete, init_complete, pong.
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace matrixcli { namespace gomuks {

// Pinned upstream protocol revision (see header comment).
inline const char* kGomuksJsoncmdPinned = "go.mau.fi/gomuks v0.2608.x";

namespace cmd {
inline constexpr const char* kGetState = "get_state";
inline constexpr const char* kPing = "ping";
inline constexpr const char* kPong = "pong";
inline constexpr const char* kResponse = "response";
inline constexpr const char* kError = "error";
inline constexpr const char* kRunID = "run_id";
inline constexpr const char* kClientState = "client_state";
inline constexpr const char* kSyncStatus = "sync_status";
inline constexpr const char* kSyncComplete = "sync_complete";
inline constexpr const char* kSendComplete = "send_complete";
inline constexpr const char* kInitComplete = "init_complete";
} // namespace cmd

struct JsonEnvelope {
    std::string command;
    int64_t requestId = 0;
    nlohmann::json data = nlohmann::json();
};

inline std::string formatCommand(const std::string& name, int64_t requestId,
                                 const nlohmann::json& data) {
    nlohmann::json j;
    j["command"] = name;
    j["request_id"] = requestId;
    j["data"] = data.is_null() ? nlohmann::json::object() : data;
    return j.dump();
}

// Parses one envelope; tolerates missing/null data (some events and the
// dataless get_state reply carry none).
inline bool parseEnvelope(const std::string& text, JsonEnvelope& out,
                          std::string& error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("bad jsoncmd envelope: ") + e.what();
        return false;
    }
    if (!j.is_object() || !j.contains("command") ||
        !j["command"].is_string()) {
        error = "bad jsoncmd envelope: missing string command";
        return false;
    }
    out.command = j["command"].get<std::string>();
    out.requestId = (j.contains("request_id") && j["request_id"].is_number())
                        ? j["request_id"].get<int64_t>()
                        : 0;
    out.data = (j.contains("data") && !j["data"].is_null())
                   ? j["data"]
                   : nlohmann::json::object();
    return true;
}

}} // namespace matrixcli::gomuks
