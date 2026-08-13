// src/agent_commands.cpp — LLM, agent and typing commands on vendored desktop
// modules (lib/ecore/native: llm.cpp, agent_executor.cpp, typing_*).
//
//   matrixcli llm <prompt>            — single LLM completion (OpenAI/Anthropic)
//   matrixcli agent <task> [--room X] — agentic loop with Matrix tools
//   matrixcli typing <room>           — who is typing (one-shot sync)
#include "commands.hpp"
#include "pcore.hpp"
#include "globals.hpp"
#include "../lib/database/db.hpp"
#include "agent_tools.hpp"
#include "core/http_client.hpp"
#include <progressive/llm.hpp>
#include <progressive/agent_executor.hpp>
#include <progressive/typing_indicator.hpp>
#include <progressive/typing_utils.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

using namespace matrixcli;

// "Key: value\nKey2: value2" -> unordered_map
static std::unordered_map<std::string, std::string> parseHeaders(const std::string& h) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream ss(h);
    std::string line;
    while (std::getline(ss, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            return s;
        };
        std::string k = trim(line.substr(0, colon));
        std::string v = trim(line.substr(colon + 1));
        if (!k.empty()) out[k] = v;
    }
    return out;
}

static progressive::LlmConfig llmConfigFromArgs(const cli::Args& args) {
    progressive::LlmConfig cfg;
    std::string prov = args.options.count("provider") ? args.options.at("provider") : "openai";
    cfg.provider = (prov == "anthropic") ? progressive::LlmProvider::Anthropic
                                         : progressive::LlmProvider::OpenAI;
    cfg.apiToken = args.options.count("token") ? args.options.at("token") : "";
    cfg.model = args.options.count("model") ? args.options.at("model")
        : (cfg.provider == progressive::LlmProvider::Anthropic
               ? "claude-3-haiku-20240307" : "gpt-4o-mini");
    cfg.apiEndpoint = args.options.count("endpoint") ? args.options.at("endpoint")
        : (cfg.provider == progressive::LlmProvider::Anthropic
               ? "https://api.anthropic.com/v1/messages"
               : "https://api.openai.com/v1/chat/completions");
    if (args.options.count("system")) cfg.systemPrompt = args.options.at("system");
    if (args.options.count("temperature")) { try { cfg.temperature = std::stof(args.options.at("temperature")); } catch (...) {} }
    if (args.options.count("max-tokens")) { try { cfg.maxTokens = std::stoi(args.options.at("max-tokens")); } catch (...) {} }
    return cfg;
}

static progressive::LlmResponse llmCall(const progressive::LlmConfig& cfg, const std::string& prompt) {
    // Request building is the vendored desktop code; response parsing uses
    // nlohmann instead of progressive::parseLlmResponse — the desktop's
    // hand-rolled parseJsonStringValue truncates nested JSON (choices is an
    // array), so it never extracts content from real OpenAI/Anthropic bodies.
    std::string body = progressive::buildLlmRequestBody(cfg, prompt);
    auto headers = parseHeaders(progressive::buildLlmHeaders(cfg));
    auto resp = progressive::desktop::httpPost(cfg.apiEndpoint, body, headers, 60000);

    progressive::LlmResponse r;
    r.statusCode = resp.statusCode;
    if (resp.statusCode != 200) {
        try {
            auto err = nlohmann::json::parse(resp.body);
            r.errorMessage = err.value("error", nlohmann::json::object()).value("message", "HTTP " + std::to_string(resp.statusCode));
        } catch (...) {
            r.errorMessage = resp.body.empty() ? ("HTTP " + std::to_string(resp.statusCode)) : resp.body.substr(0, 200);
        }
        return r;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (cfg.provider == progressive::LlmProvider::Anthropic) {
            auto& content = j["content"];
            if (content.is_array() && !content.empty()) r.text = content[0].value("text", "");
        } else {
            r.text = j.value("choices", nlohmann::json::array())[0].value("message", nlohmann::json::object()).value("content", "");
        }
        r.success = !r.text.empty();
        if (!r.success) r.errorMessage = "No content in response";
        if (j.contains("usage")) r.tokensUsed = j["usage"].value("total_tokens", 0);
    } catch (const std::exception& e) {
        r.errorMessage = std::string("response parse failed: ") + e.what();
    }
    return r;
}

static std::string readPrompt(const cli::Args& args) {
    std::string prompt;
    if (!args.positional.empty()) {
        for (size_t i = 0; i < args.positional.size(); i++) {
            if (i) prompt += " ";
            prompt += args.positional[i];
        }
    } else if (!isatty(STDIN_FILENO)) {
        std::string l;
        while (std::getline(std::cin, l)) { prompt += l; prompt += "\n"; }
    }
    return prompt;
}

int cmdLlm(const cli::Args& args) {
    bool json_out = args.options.count("json");
    std::string prompt = readPrompt(args);
    if (prompt.empty()) {
        std::cerr << "Usage: matrixcli llm <prompt> | echo <prompt> | matrixcli llm [--provider openai|anthropic] [--token t] [--model m] [--endpoint URL] [--system s] [--temperature x] [--max-tokens n] [--json]" << std::endl;
        return 1;
    }
    auto cfg = llmConfigFromArgs(args);
    if (cfg.apiToken.empty()) {
        std::cerr << "Error: --token required" << std::endl;
        return 1;
    }
    auto r = llmCall(cfg, prompt);
    if (json_out) {
        nlohmann::json j;
        j["ok"] = r.success;
        j["text"] = r.text;
        j["error"] = r.errorMessage;
        j["status"] = r.statusCode;
        j["tokens"] = r.tokensUsed;
        std::cout << j.dump() << std::endl;
    } else if (r.success) {
        std::cout << r.text << std::endl;
    } else {
        std::cerr << "LLM error: " << r.errorMessage << " (HTTP " << r.statusCode << ")" << std::endl;
        return 1;
    }
    return 0;
}

// ---- Agent tool executors (return JSON result strings) ----
static std::string toolReadMessages(const std::string& roomId, int limit, const std::string& before) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return R"({"error":"cache unavailable"})";
    auto events = dbi.getEvents(roomId, limit, before);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& e : events) {
        nlohmann::json j;
        j["sender"] = e.sender;
        j["body"] = e.content.value("body", "");
        j["ts"] = e.origin_server_ts;
        arr.push_back(j);
    }
    return arr.dump();
}

static std::string toolSendMessage(const std::string& roomId, const std::string& text) {
    auto r = pcore::core().client->sendMessage(roomId, text, "m.text");
    return r.ok ? (nlohmann::json{{"event_id", r.data}}).dump()
                : (nlohmann::json{{"error", r.error.message}}).dump();
}

static std::string toolSearchMessages(const std::string& roomId, const std::string& query) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return R"({"error":"cache unavailable"})";
    auto rows = dbi.search(query, 20);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& r : rows) {
        if (!roomId.empty() && r.value("room_id", "") != roomId) continue;
        nlohmann::json j;
        j["sender"] = r.value("sender", "");
        j["body"] = r.value("content", nlohmann::json::object()).value("body", "");
        j["ts"] = r.value("origin_server_ts", 0);
        arr.push_back(j);
    }
    return arr.dump();
}

static std::string toolListUsers(const std::string& roomId) {
    auto r = pcore::core().client->getRoomMembers(roomId);
    if (!r.ok) return R"({"error":"members lookup failed"})";
    try {
        auto j = nlohmann::json::parse(r.data);
        nlohmann::json arr = nlohmann::json::array();
        for (auto& [uid, info] : j["chunk"].items()) {
            (void)uid;
            arr.push_back(nlohmann::json{{"user_id", info.value("user_id", "")},
                                         {"displayname", info.value("displayname", "")}});
        }
        return arr.dump();
    } catch (...) { return r.data; }
}

static std::string toolGetUserInfo(const std::string& userId) {
    auto r = pcore::core().client->getUserProfile(userId);
    return r.ok ? r.data : R"({"error":"profile lookup failed"})";
}

static std::string toolReact(const std::string& roomId, const std::string& eventId, const std::string& emoji) {
    auto r = pcore::core().client->sendReaction(roomId, eventId, emoji);
    return r.ok ? R"({"ok":true})" : R"({"ok":false})";
}

static std::string toolGetRoomInfo(const std::string& roomId) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return R"({"error":"cache unavailable"})";
    for (auto& r : dbi.listRooms()) {
        if (r.value("room_id", "") == roomId) return r.dump();
    }
    return nlohmann::json{{"room_id", roomId}}.dump();
}

static std::string toolSendDm(const std::string& userId, const std::string& text) {
    auto& client = pcore::core().client;
    auto dm = client->startDirectMessage(userId, "cli");
    if (!dm.ok) return (nlohmann::json{{"error", dm.error.message}}).dump();
    auto r = client->sendMessage(dm.data, text, "m.text");
    return r.ok ? (nlohmann::json{{"room_id", dm.data}, {"event_id", r.data}}).dump()
                : (nlohmann::json{{"error", r.error.message}}).dump();
}

// Execute one tool call; returns the JSON result.
static std::string executeTool(const progressive::AgentToolCall& call) {
    std::string name = call.toolName;
    try {
        auto args = nlohmann::json::parse(call.argumentsJson);
        if (name == "read_messages")
            return toolReadMessages(args.value("room_id", ""), args.value("limit", 20), args.value("before_event_id", ""));
        if (name == "send_message")
            return toolSendMessage(args.value("room_id", ""), args.value("text", ""));
        if (name == "search_messages")
            return toolSearchMessages(args.value("room_id", ""), args.value("query", ""));
        if (name == "list_users")
            return toolListUsers(args.value("room_id", ""));
        if (name == "get_user_info")
            return toolGetUserInfo(args.value("user_id", ""));
        if (name == "react_to_message")
            return toolReact(args.value("room_id", ""), args.value("event_id", ""), args.value("emoji", "👍"));
        if (name == "send_direct_message")
            return toolSendDm(args.value("user_id", ""), args.value("text", ""));
        if (name == "get_room_info")
            return toolGetRoomInfo(args.value("room_id", ""));
        return (nlohmann::json{{"error", "unknown tool: " + name}}).dump();
    } catch (const std::exception& e) {
        return (nlohmann::json{{"error", std::string("bad arguments: ") + e.what()}}).dump();
    }
}

// Build room context (recent messages) for the agent system prompt.
static std::string buildRoomContext(const std::string& roomId, int maxMessages) {
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

int cmdAgent(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    bool json_out = args.options.count("json");
    bool verbose = args.options.count("verbose");
    std::string task = readPrompt(args);
    if (task.empty()) {
        std::cerr << "Usage: matrixcli agent <task> [--room <id|name>] [--provider openai|anthropic] [--token t] [--model m] [--max-iterations n] [--json] [--verbose]" << std::endl;
        return 1;
    }
    auto cfg = llmConfigFromArgs(args);
    if (cfg.apiToken.empty()) {
        std::cerr << "Error: --token required" << std::endl;
        return 1;
    }

    progressive::AgentConfig agentCfg;
    if (args.options.count("max-iterations")) { try { agentCfg.maxIterations = std::stoi(args.options.at("max-iterations")); } catch (...) {} }
    agentCfg.verbose = verbose;
    agentCfg.systemPrompt = progressive::buildAgentSystemPrompt(agentCfg);

    // Room context (best-effort).
    std::string roomId;
    std::string roomName = args.options.count("room") ? args.options.at("room") : "";
    if (!roomName.empty()) {
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                if (id == roomName || r.value("name", "") == roomName || r.value("name", "").find(roomName) == 0) { roomId = id; break; }
            }
        }
        if (roomId.empty()) roomId = roomName;
    }

    progressive::AgentState state;
    state.task = task;
    state.roomId = roomId;
    state.userId = pcore::core().client->account().userId;
    if (!roomId.empty()) state.roomContext = buildRoomContext(roomId, 50);

    int iterations = 0;
    bool errored = false;
    std::string lastError;

    while (!progressive::shouldStopAgent(state, agentCfg)) {
        std::string prompt = progressive::buildAgentUserPrompt(state, agentCfg);
        if (verbose) std::cerr << "[agent] iteration " << state.iteration << ": calling LLM..." << std::endl;
        auto r = llmCall(cfg, prompt);
        if (!r.success) {
            lastError = r.errorMessage;
            state.hasError = true;
            state.errorMessage = r.errorMessage;
            errored = true;
            break;
        }
        state = progressive::processAgentIteration(state, r.text);

        if (!state.pendingToolCalls.empty()) {
            for (auto& call : state.pendingToolCalls) {
                if (verbose) std::cerr << "[agent] tool: " << call.toolName << " " << call.argumentsJson << std::endl;
                std::string result = executeTool(call);
                std::string toolText = progressive::buildToolResultText(call.toolName, result);
                state.conversationHistory.push_back(toolText);
            }
            state.pendingToolCalls.clear();
        }
        iterations++;
        if (iterations >= agentCfg.maxIterations) break;
    }

    if (json_out) {
        nlohmann::json j;
        j["ok"] = !errored && state.isComplete;
        j["answer"] = state.finalAnswer;
        j["iterations"] = state.iteration;
        j["error"] = errored ? lastError : "";
        std::cout << j.dump() << std::endl;
    } else if (errored) {
        std::cerr << "Agent error: " << lastError << std::endl;
        return 1;
    } else if (!state.finalAnswer.empty()) {
        std::cout << state.finalAnswer << std::endl;
    } else {
        std::cout << "Agent finished without a final answer." << std::endl;
    }
    return errored ? 1 : 0;
}

int cmdTyping(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli typing <room>" << std::endl;
        return 1;
    }
    std::string target = args.positional[0];
    bool json_out = args.options.count("json");

    // One-shot sync; capture typing users for the target room.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::string captured;
    pcore::startSync([&](const progressive::desktop::FastSyncResponse& resp) {
        for (auto& [roomIdView, room] : resp.joinedRooms) {
            if (std::string(roomIdView) == target || !room.typingUsers.empty()) {
                if (captured.empty()) {
                    for (auto& u : room.typingUsers) {
                        if (!captured.empty()) captured += ",";
                        captured += std::string(u);
                    }
                }
            }
        }
        done->store(true);
    });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!done->load() && matrixcli::g_interrupted.load()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    pcore::stopSync();

    if (json_out) {
        nlohmann::json j;
        j["room"] = target;
        if (!captured.empty()) {
            std::vector<std::string> users;
            std::string u;
            std::istringstream ss(captured);
            while (std::getline(ss, u, ',')) users.push_back(u);
            j["typing"] = users;
        } else {
            j["typing"] = nlohmann::json::array();
        }
        std::cout << j.dump() << std::endl;
    } else if (!captured.empty()) {
        // typing_indicator.hpp helpers live in the global namespace.
        std::string json = "{\"user_ids\":[\"" + captured + "\"]}";
        std::cout << formatTypingList(json) << std::endl;
    } else {
        std::cout << "Nobody is typing in " << target << "." << std::endl;
    }
    return 0;
}

// ---- agent-code: the LOCAL coding agent (opencode-style) ----

static int cmdAgentCode(const cli::Args& args) {
    using namespace matrixcli::agenttools;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli agent-code <prompt> [--provider openai|anthropic]"
                     " [--endpoint url] [--model m] [--key k]"
                     " [--trust allow|ask|deny] [--verbose]" << std::endl;
        return 1;
    }
    Config cfg;
    db::Database dbi;
    if (dbi.open("matrixcli.db")) {
        cfg.provider = dbi.getSetting("agent_provider", "openai");
        cfg.endpoint = dbi.getSetting("agent_endpoint", "");
        cfg.model = dbi.getSetting("agent_model", "");
        cfg.key = dbi.getSetting("agent_key", "");
        cfg.trust = dbi.getSetting("agent_trust", "ask");
        auto loadCsv = [&](const std::string& k, std::vector<std::string>& out) {
            std::string v = dbi.getSetting(k, "");
            std::string cur;
            for (char ch : v) {
                if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            if (!cur.empty()) out.push_back(cur);
        };
        loadCsv("agent_allow", cfg.allowPrefixes);
        loadCsv("agent_deny", cfg.denyPrefixes);
    }
    if (args.options.count("provider")) cfg.provider = args.options.at("provider");
    if (args.options.count("endpoint")) cfg.endpoint = args.options.at("endpoint");
    if (args.options.count("model")) cfg.model = args.options.at("model");
    if (args.options.count("key")) cfg.key = args.options.at("key");
    if (args.options.count("trust")) cfg.trust = args.options.at("trust");
    if (cfg.key.empty()) {
        const char* env = cfg.provider == "anthropic"
                              ? std::getenv("ANTHROPIC_API_KEY")
                              : std::getenv("OPENAI_API_KEY");
        if (env && *env) cfg.key = env;
    }
    if (cfg.model.empty()) {
        cfg.model = cfg.provider == "anthropic" ? "claude-3-5-haiku-20241022"
                                                : "gpt-4o-mini";
    }
    if (cfg.key.empty()) {
        std::cerr << "Error: no API key — pass --key, save it in the settings"
                     " (agent config key <k>), or export OPENAI_API_KEY/"
                     "ANTHROPIC_API_KEY" << std::endl;
        return 1;
    }
    char cwdbuf[4096];
    if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;
    std::string prompt;
    for (const auto& p : args.positional) prompt += (prompt.empty() ? "" : " ") + p;
    bool verbose = args.options.count("verbose");
    std::vector<Message> history;
    Result res = run(cfg, prompt, history,
        [&](const std::string& cmd) -> bool {
            std::cout << "run: " << cmd << " [y/N] " << std::flush;
            std::string ans;
            std::getline(std::cin, ans);
            return ans == "y" || ans == "Y";
        },
        [&](const std::string& questionsJson) -> std::string {
            nlohmann::json qs;
            try { qs = nlohmann::json::parse(questionsJson); }
            catch (...) { return "error: bad questions JSON"; }
            std::string out;
            for (const auto& q : qs.value("questions", nlohmann::json::array())) {
                std::cout << "  Q: " << q.value("question", "?") << std::endl;
                auto opts = q.value("options", nlohmann::json::array());
                for (size_t i = 0; i < opts.size(); ++i) {
                    std::cout << "    " << i + 1 << ") "
                              << opts[i].value("label", "") << std::endl;
                }
                std::cout << "  answer> " << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                out += "Q: " + q.value("question", "?") + "\nA: " + ans + "\n";
            }
            return out.empty() ? "(no questions answered)" : out;
        },
        [&](const std::string& l) {
            if (verbose) std::cout << l << std::endl;
        });
    if (!res.ok) {
        std::cerr << "agent error: " << res.error << std::endl;
        return 1;
    }
    std::cout << res.text << std::endl;
    return 0;
}

void registerAgentCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("llm", cmdLlm, "LLM completion: llm <prompt> [--provider] [--token] [--model]");
    reg.registerCli("agent", cmdAgent, "Agentic loop with Matrix tools: agent <task> [--room X] [--token t]");
    reg.registerCli("agent-code", cmdAgentCode, "Local coding agent: agent-code <prompt> [--provider] [--model] [--trust allow|ask|deny]");
    reg.registerCli("typing", cmdTyping, "Who is typing: typing <room>");
}
