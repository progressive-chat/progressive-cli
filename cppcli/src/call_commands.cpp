// src/call_commands.cpp — the Matrix VoIP signaling (stage 1: the call
// state machine over the m.call.* to-device events, no media yet).
//
//   progressive-cli call <@user:server>   — the invite (m.call.invite to-device)
//   progressive-cli call answer <call_id> — the m.call.answer
//   progressive-cli call hangup <call_id> — the m.call.hangup
//   progressive-cli call status          — the known calls (the state store)
//   progressive-cli call wait            — listen: the invites/answers/hangups
//
// The state lives in ~/.local/share/matrixcli/calls.json (XDG). The
// WebRTC media plane is the next stage — the SDPs are empty for now, the
// signaling is complete.
#include "commands.hpp"
#include "globals.hpp"
#include "pcore.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace matrixcli;

namespace {

std::string callsPath() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base =
        (xdg && *xdg) ? std::filesystem::path(xdg)
                      : (home && *home) ? std::filesystem::path(home) / ".local/share"
                                        : std::filesystem::path(".");
    return (base / "matrixcli/calls.json").string();
}

nlohmann::json loadCalls() {
    std::ifstream f(callsPath());
    if (!f) return nlohmann::json::object();
    try {
        nlohmann::json j;
        f >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

void saveCalls(const nlohmann::json& j) {
    std::filesystem::create_directories(
        std::filesystem::path(callsPath()).parent_path());
    std::ofstream f(callsPath(), std::ios::trunc);
    f << j.dump(2);
}

std::string newTxnId() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "mc" + std::to_string(now);
}

// The to-device send to ALL the peer's devices (the "*" wildcard).
bool sendCallEvent(const std::string& type, const std::string& peer,
                   const nlohmann::json& content) {
    auto& client = pcore::core().client;
    if (!client) return false;
    nlohmann::json body = {{"messages", {{peer, {{"*", content}}}}}};
    const auto r = client->sendToDevice(type, newTxnId(), body.dump());
    return r.ok;
}

void usage() {
    std::cerr << "Usage: progressive-cli call <@user:server>\n"
              << "       progressive-cli call answer <call_id>\n"
              << "       progressive-cli call hangup <call_id>\n"
              << "       progressive-cli call status\n"
              << "       progressive-cli call wait\n";
}

} // namespace

int cmdCall(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        usage();
        return 1;
    }
    const std::string sub = args.positional[0];
    nlohmann::json calls = loadCalls();

    if (sub == "status") {
        if (calls.empty()) {
            std::cout << "No calls (the store: " << callsPath() << ")"
                      << std::endl;
            return 0;
        }
        for (auto& [callId, call] : calls.items()) {
            std::cout << "  " << callId.substr(0, 20) << "...  "
                      << call.value("direction", "?") << "  "
                      << call.value("state", "?") << "  "
                      << call.value("peer", "") << std::endl;
        }
        return 0;
    }

    if (sub == "answer" || sub == "hangup" || sub == "reject") {
        if (args.positional.size() < 2) {
            usage();
            return 1;
        }
        const std::string callId = args.positional[1];
        auto it = calls.find(callId);
        if (it == calls.end()) {
            std::cerr << "No such call: " << callId << " (call status)"
                      << std::endl;
            return 1;
        }
        const std::string peer = it->value("peer", "");
        nlohmann::json content = {{"call_id", callId}, {"version", 1}};
        if (sub == "answer") {
            content["answer"] = {{"type", "answer"}, {"sdp", ""}};
            content["capabilities"] = {{"m.call.opposed_sdp_streams", true}};
        } else {
            content["reason"] = sub == "reject" ? "reject" : "user";
        }
        if (!sendCallEvent(sub == "answer" ? "m.call.answer" : "m.call.hangup",
                           peer, content)) {
            std::cerr << "The send failed (is the session live?)" << std::endl;
            return 1;
        }
        it.value()["state"] = sub == "answer" ? "connected" : "ended";
        saveCalls(calls);
        std::cout << (sub == "answer" ? "Answered " : "Hung up ") << callId
                  << " (" << peer << ")" << std::endl;
        return 0;
    }

    if (sub == "wait") {
        // The listener: the sync loop delivers the to-device events.
        pcore::startSync([](const progressive::desktop::FastSyncResponse& resp) {
            for (const auto& evt : resp.toDeviceEventList) {
                if (evt.type != "m.call.invite" && evt.type != "m.call.answer" &&
                    evt.type != "m.call.hangup" && evt.type != "m.call.candidates")
                    continue;
                nlohmann::json content;
                try {
                    content = nlohmann::json::parse(std::string(evt.contentJson));
                } catch (...) {
                    continue;
                }
                const std::string callId = content.value("call_id", "");
                if (callId.empty()) continue;
                nlohmann::json calls = loadCalls();
                auto& entry = calls[callId];
                entry["peer"] = std::string(evt.senderId);
                if (evt.type == "m.call.invite") {
                    entry["direction"] = "incoming";
                    entry["state"] = "invited";
                    std::cout << "\nIncoming call " << callId << " from "
                              << evt.senderId << " — progressive-cli call answer "
                              << callId << std::endl;
                } else if (evt.type == "m.call.answer") {
                    entry["direction"] = entry.value("direction", "outgoing");
                    entry["state"] = "connected";
                    std::cout << "\nCall " << callId << " answered by "
                              << evt.senderId << std::endl;
                } else if (evt.type == "m.call.hangup") {
                    entry["state"] = "ended";
                    std::cout << "\nCall " << callId << " hung up by "
                              << evt.senderId << " ("
                              << content.value("reason", "?") << ")"
                              << std::endl;
                }
                saveCalls(calls);
            }
        });
        std::cout << "Listening for the call events (Ctrl+C to stop)..."
                  << std::endl;
        while (matrixcli::g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        pcore::stopSync();
        return 0;
    }

    // The outgoing call: call <@user:server>.
    const std::string peer = sub;
    const std::string callId = "mc" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    nlohmann::json content = {
        {"call_id", callId},
        {"version", 1},
        {"lifetime", 60000},
        {"offer", {{"type", "offer"}, {"sdp", ""}}},
        {"capabilities", {{"m.call.opposed_sdp_streams", true}}}};
    if (!sendCallEvent("m.call.invite", peer, content)) {
        std::cerr << "The invite failed (is the session live?)" << std::endl;
        return 1;
    }
    calls[callId] = {{"peer", peer},
                     {"direction", "outgoing"},
                     {"state", "invite_sent"}};
    saveCalls(calls);
    std::cout << "Calling " << peer << " (" << callId << ")\n"
              << "The signaling is live — the WebRTC media is the next stage."
              << std::endl;
    return 0;
}

void registerCallCommands() {
    CommandRegistry::instance().registerCli(
        "call", cmdCall,
        "Matrix VoIP signaling: call <@user> | answer <id> | hangup <id> | status | wait");
}
