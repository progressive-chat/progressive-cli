#include "matrix_agent.hpp"

#include "agent_tools.hpp"
#include "globals.hpp"
#include "pcore.hpp"

#include "../lib/database/db.hpp"
#include "../lib/http/http.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/events.hpp"
#include "../lib/util/llm_sse.hpp"

#include <progressive/agent_executor.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace matrixcli { namespace matrixagent {

using nlohmann::json;

namespace {

std::string errJson(const std::string& e) { return json{{"error", e}}.dump(); }

// ---- Ecore backend (the pcore session, used by the CLI) ----

class EcoreBackend : public Backend {
public:
    std::string sendMessage(const std::string& roomId, const std::string& text) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto r = c->sendMessage(roomId, text, "m.text");
        return r.ok ? json{{"event_id", r.data}}.dump() : errJson(r.error.message);
    }
    std::string editMessage(const std::string& roomId, const std::string& eventId,
                            const std::string& newText) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto r = c->editMessage(roomId, eventId, newText);
        return r.ok ? json{{"success", true}}.dump() : errJson(r.error.message);
    }
    std::string listUsers(const std::string& roomId) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto r = c->getRoomMembers(roomId);
        if (!r.ok) return errJson("members lookup failed");
        try {
            auto j = json::parse(r.data);
            json arr = json::array();
            for (auto& [uid, info] : j["chunk"].items()) {
                (void)uid;
                arr.push_back({{"user_id", info.value("user_id", "")},
                               {"displayname", info.value("displayname", "")}});
            }
            return arr.dump();
        } catch (...) { return r.data; }
    }
    std::string getUserInfo(const std::string& userId) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto r = c->getUserProfile(userId);
        return r.ok ? r.data : errJson("profile lookup failed");
    }
    std::string react(const std::string& roomId, const std::string& eventId,
                      const std::string& emoji) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto r = c->sendReaction(roomId, eventId, emoji);
        return r.ok ? json{{"success", true}}.dump() : json{{"success", false}}.dump();
    }
    std::string sendDirectMessage(const std::string& userId, const std::string& text) override {
        auto& c = pcore::core().client;
        if (!c) return errJson("no matrix session");
        auto dm = c->startDirectMessage(userId, true);
        if (!dm.ok) return errJson(dm.error.message);
        auto r = c->sendMessage(dm.data, text, "m.text");
        return r.ok ? json{{"room_id", dm.data}, {"event_id", r.data}}.dump()
                    : errJson(r.error.message);
    }
    std::string selfUserId() const override {
        auto& c = pcore::core().client;
        return c ? c->account().userId : std::string();
    }
};

// ---- lib/matrix backend (the TUI's own logged-in client) ----

class MatrixBackend : public Backend {
public:
    explicit MatrixBackend(matrix::Client* client) : _client(client) {}

    // The live operations need a logged-in session. In the demo TUI and
    // the offline agent mode (`matrixcli tui agent`) the client exists but
    // has no token — the tools report it clearly and the agent continues
    // with the cache-backed tools (read/search/room info).
    bool haveSession() const { return _client && _client->isLoggedIn(); }

    std::string sendMessage(const std::string& roomId, const std::string& text) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        std::string ev = _client->sendMessage(roomId, text, "m.text");
        return ev.empty() ? errJson("send failed") : json{{"event_id", ev}}.dump();
    }
    std::string editMessage(const std::string& roomId, const std::string& eventId,
                            const std::string& newText) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        try {
            json content = {{"msgtype", "m.text"}, {"body", "* " + newText},
                            {"m.new_content", {{"msgtype", "m.text"}, {"body", newText}}},
                            {"m.relates_to", {{"event_id", eventId}, {"rel_type", "m.replace"}}}};
            std::string ev = _client->sendEvent(roomId, "m.room.message", content);
            return ev.empty() ? errJson("edit failed") : json{{"success", true}}.dump();
        } catch (...) { return errJson("edit failed"); }
    }
    std::string listUsers(const std::string& roomId) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        json arr = json::array();
        for (auto& m : _client->getRoomMembers(roomId)) {
            if (m.content.value("membership", "") != "join") continue;
            arr.push_back({{"user_id", m.state_key},
                           {"displayname", m.content.value("displayname", "")}});
        }
        return arr.dump();
    }
    std::string getUserInfo(const std::string& userId) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        matrix::UserInfo u = _client->getProfile(userId);
        return json{{"user_id", u.user_id}, {"displayname", u.display_name},
                    {"avatar_url", u.avatar_url}}.dump();
    }
    std::string react(const std::string& roomId, const std::string& eventId,
                      const std::string& emoji) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        try {
            std::string ev = _client->sendReaction(roomId, eventId, emoji);
            return ev.empty() ? json{{"success", false}}.dump()
                              : json{{"success", true}}.dump();
        } catch (...) { return json{{"success", false}}.dump(); }
    }
    std::string sendDirectMessage(const std::string& userId, const std::string& text) override {
        if (!haveSession()) return errJson("no matrix session (offline or demo mode)");
        try {
            std::string room = _client->createRoom("", "", true, {userId});
            if (room.empty()) return errJson("dm create failed");
            std::string ev = _client->sendMessage(room, text, "m.text");
            return ev.empty() ? errJson("send failed")
                              : json{{"room_id", room}, {"event_id", ev}}.dump();
        } catch (...) { return errJson("dm failed"); }
    }
    std::string selfUserId() const override {
        return _client ? _client->userId() : std::string();
    }

private:
    matrix::Client* _client = nullptr;
};

} // namespace

std::unique_ptr<Backend> makeEcoreBackend() {
    return std::make_unique<EcoreBackend>();
}

std::unique_ptr<Backend> makeMatrixBackend(matrix::Client* client) {
    return std::make_unique<MatrixBackend>(client);
}

// ---- config ----

void applyDefaults(Config& cfg) {
    // 1. agent.json — the base config (the same file the coding agent
    //    uses). It only fills the fields the caller left empty.
    agenttools::Config file;
    if (agenttools::loadAgentConfig(file)) {
        if (!file.provider.empty() && cfg.provider.empty()) cfg.provider = file.provider;
        if (!file.endpoint.empty() && cfg.endpoint.empty()) cfg.endpoint = file.endpoint;
        if (!file.model.empty() && cfg.model.empty()) cfg.model = file.model;
        if (!file.key.empty() && cfg.key.empty()) cfg.key = file.key;
        if (!file.proxy.empty() && cfg.proxy.empty()) cfg.proxy = file.proxy;
    }
    // 2. Provider presets (openai, anthropic, ollama, deepseek, mimo, ...).
    if (!cfg.provider.empty()) {
        agenttools::Config preset;
        if (agenttools::applyProviderPreset(preset, cfg.provider)) {
            cfg.provider = preset.provider;  // the wire format (openai|anthropic)
            if (cfg.endpoint.empty()) cfg.endpoint = preset.endpoint;
            if (cfg.model.empty()) cfg.model = preset.model;
        } else if (cfg.provider != "anthropic") {
            cfg.provider = "openai";  // unknown names use the OpenAI-compatible wire
        }
    }
    // 3. Final defaults.
    if (cfg.provider.empty()) cfg.provider = "openai";
    if (cfg.endpoint.empty()) {
        cfg.endpoint = cfg.provider == "anthropic" ? "https://api.anthropic.com"
                                                   : "https://api.openai.com";
    }
    if (cfg.model.empty()) {
        cfg.model = cfg.provider == "anthropic" ? "claude-3-haiku-20240307"
                                                : "gpt-4o-mini";
    }
    // 4. The environment keys.
    if (cfg.key.empty()) {
        const char* env = cfg.provider == "anthropic"
                              ? std::getenv("ANTHROPIC_API_KEY")
                              : std::getenv("OPENAI_API_KEY");
        if (env && *env) cfg.key = env;
    }
}

// ---- LLM transport ----

namespace {

std::string chatUrl(const Config& cfg) {
    std::string base = cfg.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (cfg.provider == "anthropic") {
        if (base.size() > 11 && base.rfind("/v1/messages") == base.size() - 11)
            return base;
        return base + "/v1/messages";
    }
    if (base.size() > 17 && base.rfind("/chat/completions") == base.size() - 17)
        return base;
    return base + "/v1/chat/completions";
}

namespace {

} // namespace

// One chat completion. Returns the Completion (text + usage metadata);
// on failure returns the unexpected error.
std::expected<Completion, std::string>
llmCall(const Config& cfg, const std::string& system,
        const std::vector<ChatMessage>& messages) {
    Completion out;
    http::Client httpClient;
    httpClient.setTimeout(600);  // the reasoning models think for minutes between chunks
    if (!cfg.proxy.empty()) {
        auto colon = cfg.proxy.rfind(':');
        if (colon != std::string::npos) {
            http::ProxyConfig pc;
            pc.type = http::ProxyType::SOCKS5;
            pc.host = cfg.proxy.substr(0, colon);
            try { pc.port = std::stoi(cfg.proxy.substr(colon + 1)); } catch (...) {}
            if (pc.enabled()) httpClient.setProxy(pc);
        }
    }

    json body;
    std::map<std::string, std::string> headers;
    if (cfg.provider == "anthropic") {
        json msgs = json::array();
        for (const auto& m : messages) {
            msgs.push_back({{"role", m.role.empty() ? "user" : m.role},
                            {"content", m.content}});
        }
        body = {{"model", cfg.model},
                {"system", system},
                {"messages", msgs},
                {"max_tokens", cfg.maxTokens > 0 ? cfg.maxTokens : 8192}};
        headers["x-api-key"] = cfg.key;
        headers["anthropic-version"] = "2023-06-01";
    } else {
        json msgs = json::array();
        if (!system.empty())
            msgs.push_back({{"role", "system"}, {"content", system}});
        for (const auto& m : messages) {
            msgs.push_back({{"role", m.role.empty() ? "user" : m.role},
                            {"content", m.content}});
        }
        body = {{"model", cfg.model}, {"messages", msgs},
                {"temperature", cfg.temperature}};
        if (cfg.topP > 0) body["top_p"] = cfg.topP;
        if (!cfg.reasoning.empty()) body["reasoning_effort"] = cfg.reasoning;
        // 0 = unlimited: the field is omitted, the provider decides.
        if (cfg.maxTokens > 0) body["max_tokens"] = cfg.maxTokens;
        headers["authorization"] = "Bearer " + cfg.key;
    }
    headers["content-type"] = "application/json";

    out.model = cfg.model;
    out.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count();
    http::Response resp = httpClient.post(chatUrl(cfg), body.dump(), headers);
    if (!resp.ok()) {
        try {
            auto j = json::parse(resp.body);
            return std::unexpected(
                j.value("error", json::object())
                    .value("message", "HTTP " + std::to_string(resp.status_code)));
        } catch (...) {
            return std::unexpected(
                resp.body.empty() ? "HTTP " + std::to_string(resp.status_code)
                                  : resp.body.substr(0, 200));
        }
    }
    try {
        auto j = json::parse(resp.body);
        auto& usage = j["usage"];
        if (usage.is_object()) {
            if (cfg.provider == "anthropic") {
                out.promptTokens = usage.value("input_tokens", 0);
                out.completionTokens = usage.value("output_tokens", 0);
            } else {
                out.promptTokens = usage.value("prompt_tokens", 0);
                out.completionTokens = usage.value("completion_tokens", 0);
            }
        }
        if (cfg.provider == "anthropic") {
            auto& content = j["content"];
            if (content.is_array() && !content.empty())
                out.text = content[0].value("text", "");
        } else {
            auto choices = j.value("choices", json::array());
            if (!choices.empty() && choices[0].is_object()) {
                auto msg = choices[0].value("message", json::object());
                util::extractContent(msg, out.text, out.reasoning);
            }
        }
    out.ok = true;
    } catch (const std::exception& e) {
        return std::unexpected(std::string("response parse failed: ") + e.what());
    }
    if (out.text.empty()) {
        if (!out.reasoning.empty()) {
            return std::unexpected(
                "the model produced only reasoning ("
                + std::to_string(out.reasoning.size())
                + " chars), no answer — try --show-reasoning or --max-tokens");
        }
        return std::unexpected("no content in response — raw: "
                               + resp.body.substr(0, 220));
    }
    return out;
}

} // namespace

// ---- the cache-backed tools (shared by both backends) ----

namespace {

std::string toolReadMessages(const std::string& roomId, int limit, const std::string& before) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return errJson("cache unavailable");
    auto events = dbi.getEvents(roomId, limit, before);
    json arr = json::array();
    for (auto& e : events) {
        arr.push_back({{"sender", e.sender},
                       {"body", e.content.value("body", "")},
                       {"ts", e.origin_server_ts},
                       {"event_id", e.event_id}});
    }
    return arr.dump();
}

std::string toolSearchMessages(const std::string& roomId, const std::string& query) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return errJson("cache unavailable");
    auto rows = dbi.search(query, 20);
    json arr = json::array();
    for (auto& r : rows) {
        if (!roomId.empty() && r.value("room_id", "") != roomId) continue;
        arr.push_back({{"sender", r.value("sender", "")},
                       {"body", r.value("content", json::object()).value("body", "")},
                       {"ts", r.value("origin_server_ts", 0)}});
    }
    return arr.dump();
}

std::string toolGetRoomInfo(const std::string& roomId) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return errJson("cache unavailable");
    for (auto& r : dbi.listRooms()) {
        if (r.value("room_id", "") == roomId) return r.dump();
    }
    return json{{"room_id", roomId}}.dump();
}

std::string buildRoomContext(const std::string& roomId, int maxMessages) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return "";
    auto events = dbi.getEvents(roomId, maxMessages, "");
    std::vector<std::string> senders, bodies;
    std::vector<int64_t> ts;
    for (auto& e : events) {
        if (e.type != "m.room.message") continue;
        senders.push_back(e.sender);
        bodies.push_back(e.content.value("body", ""));
        ts.push_back(e.origin_server_ts);
    }
    return progressive::formatMessagesForAgent(senders, bodies, ts);
}

std::string executeTool(Backend& backend, const progressive::AgentToolCall& call) {
    std::string name = call.toolName;
    try {
        auto args = json::parse(call.argumentsJson);
        if (name == "read_messages")
            return toolReadMessages(args.value("room_id", ""), args.value("limit", 20),
                                    args.value("before_event_id", ""));
        if (name == "send_message")
            return backend.sendMessage(args.value("room_id", ""), args.value("text", ""));
        if (name == "edit_message")
            return backend.editMessage(args.value("room_id", ""), args.value("event_id", ""),
                                       args.value("new_text", ""));
        if (name == "search_messages")
            return toolSearchMessages(args.value("room_id", ""), args.value("query", ""));
        if (name == "list_users")
            return backend.listUsers(args.value("room_id", ""));
        if (name == "get_user_info")
            return backend.getUserInfo(args.value("user_id", ""));
        if (name == "react_to_message")
            return backend.react(args.value("room_id", ""), args.value("event_id", ""),
                                 args.value("emoji", "👍"));
        if (name == "send_direct_message")
            return backend.sendDirectMessage(args.value("user_id", ""), args.value("text", ""));
        if (name == "get_room_info")
            return toolGetRoomInfo(args.value("room_id", ""));
        return errJson("unknown tool: " + name);
    } catch (const std::exception& e) {
        return errJson(std::string("bad arguments: ") + e.what());
    }
}

std::string resolveRoomId(const std::string& roomId) {
    if (roomId.empty() || roomId.find('!') == 0) return roomId;
    // A room name or alias — resolve against the local cache.
    db::Database dbi;
    if (dbi.open("matrixcli.db")) {
        for (auto& r : dbi.listRooms()) {
            std::string id = r.value("room_id", "");
            if (id == roomId || r.value("name", "") == roomId ||
                r.value("name", "").find(roomId) == 0)
                return id;
        }
    }
    return roomId;
}

} // namespace

// ---- the agent loop ----

std::expected<Completion, std::string>
completeEx(const Config& cfg, const std::string& system,
           const std::string& prompt) {
    return llmCall(cfg, system, {{"user", prompt}});
}

std::expected<Completion, std::string>
chat(const Config& cfg, const std::string& system,
     const std::vector<ChatMessage>& messages) {
    return llmCall(cfg, system, messages);
}

std::expected<Completion, std::string>
stream(const Config& cfg, const std::string& system,
       const std::vector<ChatMessage>& messages,
       const std::function<void(const std::string&)>& onToken,
       const std::function<void(const std::string&)>& onReasoning) {
    // The Anthropic event-stream deltas take the non-streaming path for
    // now; the OpenAI-compatible wire streams.
    if (cfg.provider == "anthropic" || !onToken) {
        return llmCall(cfg, system, messages);
    }

    Completion out;
    http::Client httpClient;
    httpClient.setTimeout(600);  // the reasoning models think for minutes between chunks
    if (!cfg.proxy.empty()) {
        auto colon = cfg.proxy.rfind(':');
        if (colon != std::string::npos) {
            http::ProxyConfig pc;
            pc.type = http::ProxyType::SOCKS5;
            pc.host = cfg.proxy.substr(0, colon);
            try { pc.port = std::stoi(cfg.proxy.substr(colon + 1)); } catch (...) {}
            if (pc.enabled()) httpClient.setProxy(pc);
        }
    }

    json msgs = json::array();
    if (!system.empty())
        msgs.push_back({{"role", "system"}, {"content", system}});
    for (const auto& m : messages) {
        msgs.push_back({{"role", m.role.empty() ? "user" : m.role},
                        {"content", m.content}});
    }
    json body = {{"model", cfg.model}, {"messages", msgs},
                 {"temperature", cfg.temperature}, {"stream", true},
                 {"stream_options", {{"include_usage", true}}}};
    if (cfg.topP > 0) body["top_p"] = cfg.topP;
    if (!cfg.reasoning.empty()) body["reasoning_effort"] = cfg.reasoning;
    if (cfg.maxTokens > 0) body["max_tokens"] = cfg.maxTokens;
    std::map<std::string, std::string> headers;
    headers["authorization"] = "Bearer " + cfg.key;
    headers["content-type"] = "application/json";

    out.model = cfg.model;
    out.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count();

    std::string lastPayload;   // for the diagnostics
    std::string firstPayload;  // for the diagnostics
    int totalBytes = 0;
    int parsedEvents = 0;
    int contentEvents = 0;
    int reasoningChars = 0;
    util::SseEventAssembler sse;  // the shared, tested SSE assembly
    http::Response resp = httpClient.streamPost(
        chatUrl(cfg), body.dump(), headers,
        [&](const std::string& chunk) {
            totalBytes += static_cast<int>(chunk.size());
            sse.feed(chunk, [&](const std::string& payload) {
                lastPayload = payload;
                if (firstPayload.empty()) firstPayload = payload;
                if (cfg.debugLlm) {
                    std::cerr << "[llm] sse: "
                              << payload.substr(0, 800) << std::endl;
                }
                try {
                    auto j = json::parse(payload);
                    parsedEvents++;
                    if (j.contains("usage") && j["usage"].is_object()) {
                        auto& u = j["usage"];
                        out.promptTokens = u.value("prompt_tokens", 0);
                        out.completionTokens = u.value("completion_tokens", 0);
                    }
                    auto choices = j.value("choices", json::array());
                    if (choices.empty() || !choices[0].is_object()) return;
                    auto delta = choices[0].value("delta", json::object());
                    // The delta shapes across the providers: a string, an
                    // array of blocks, or message.content.
                    std::string piece;
                    std::string rp;
                    util::extractContent(delta, piece, rp);
                    if (piece.empty() && !delta.value("text", "").empty())
                        piece = delta.value("text", "");
                    if (piece.empty())
                        util::extractContent(
                            delta.value("message", json::object()),
                            piece, rp);
                    if (!rp.empty()) {
                        out.reasoning += rp;
                        reasoningChars += static_cast<int>(rp.size());
                        if (onReasoning) onReasoning(rp);
                    }
                    if (piece.empty()) return;
                    contentEvents++;
                    out.text += piece;
                    onToken(piece);
                } catch (const std::exception& e) {
                    if (cfg.debugLlm) {
                        std::cerr << "[llm] EXC: " << e.what() << std::endl;
                    }
                }
            });
        },
        []() {
            return !matrixcli::g_interrupted.load() ||
                   matrixcli::g_agentInterrupt.load();
        });

    if (resp.status_code < 200 || resp.status_code >= 300) {
        return std::unexpected(
            "stream failed: HTTP " + std::to_string(resp.status_code) +
            (resp.error_message.empty() ? "" : " — " + resp.error_message));
    }
    if (out.text.empty()) {
        if (!out.reasoning.empty()) {
            return std::unexpected(
                "the model streamed only reasoning ("
                + std::to_string(out.reasoning.size())
                + " chars), no answer — try --show-reasoning");
        }
        std::string err = "no content in response (stream: HTTP "
                        + std::to_string(resp.status_code) + ", "
                        + std::to_string(totalBytes) + " bytes, "
                        + std::to_string(parsedEvents) + " events, "
                        + std::to_string(contentEvents) + " content deltas, "
                        + std::to_string(reasoningChars) + " reasoning chars)";
        if (!firstPayload.empty())
            err += " — first: " + firstPayload.substr(0, 1000);
        if (!lastPayload.empty())
            err += " — last: " + lastPayload.substr(0, 1000);
        err += " — retry with --no-stream, or --debug-llm";
        return std::unexpected(err);
    }
    if (out.completionTokens == 0) out.completionTokens = out.text.size() / 4;
    return out;
}

Result run(const Config& cfg, Backend& backend, const std::string& task,
           const std::string& roomId,
           const std::function<void(const std::string&)>& log) {
    Result res;
    if (cfg.key.empty()) {
        res.error = "no API key — run './matrixcli tui agent' for the "
                    "interactive setup, or set it in ~/.config/matrixcli/"
                    "agent.json / OPENAI_API_KEY / ANTHROPIC_API_KEY";
        return res;
    }

    std::string room = resolveRoomId(roomId);

    progressive::AgentConfig acfg;
    acfg.maxIterations = cfg.maxIterations;
    acfg.verbose = cfg.verbose;
    acfg.toolsDescription = progressive::getAgentToolsSchema();
    acfg.systemPrompt = progressive::buildAgentSystemPrompt(acfg);
    if (!cfg.extraSystem.empty()) acfg.systemPrompt += "\n" + cfg.extraSystem;

    progressive::AgentState state;
    state.task = task;
    state.roomId = room;
    state.userId = backend.selfUserId();
    if (!room.empty()) state.roomContext = buildRoomContext(room, 50);

    std::string lastError;
    while (!progressive::shouldStopAgent(state, acfg)) {
        if (!matrixcli::g_interrupted.load()) {
            res.error = "interrupted (Ctrl+C)";
            return res;
        }
        if (matrixcli::g_agentInterrupt.load()) {
            res.error = "interrupted (Esc)";
            return res;
        }
        std::string prompt = progressive::buildAgentUserPrompt(state, acfg);
        if (cfg.verbose && log)
            log("[agent] iteration " + std::to_string(state.iteration + 1)
                + ": calling " + cfg.model + " ...");
        auto cres = llmCall(cfg, acfg.systemPrompt, {{"user", prompt}});
        if (!cres) {
            lastError = cres.error();
            state.hasError = true;
            state.errorMessage = cres.error();
            break;
        }
        state = progressive::processAgentIteration(state, cres->text);
        for (auto& call : state.pendingToolCalls) {
            if (cfg.verbose && log)
                log("[agent] tool: " + call.toolName + " " + call.argumentsJson);
            state.conversationHistory.push_back(
                progressive::buildToolResultText(call.toolName, executeTool(backend, call)));
        }
        state.pendingToolCalls.clear();
    }

    if (state.hasError) {
        res.error = state.errorMessage.empty() ? lastError : state.errorMessage;
        return res;
    }
    res.ok = true;
    res.answer = state.finalAnswer;
    res.iterations = state.iteration;
    return res;
}

}} // namespace matrixcli::matrixagent
