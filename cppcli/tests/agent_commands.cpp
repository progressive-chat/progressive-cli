// src/agent_commands.cpp — LLM, agent and typing commands on vendored desktop
// modules (lib/ecore/native: llm.cpp, agent_executor.cpp, typing_*).
//
//   matrixcli llm <prompt>            — single LLM completion (any provider)
//   matrixcli agent <task> [--room X] — agentic loop with Matrix tools
//   matrixcli typing <room>           — who is typing (one-shot sync)
//
// The `llm` and `agent` commands share the backend config with the TUI
// slash commands: ~/.config/matrixcli/agent.json + env keys + the CLI
// flags (the flags win).
#include "commands.hpp"
#include "pcore.hpp"
#include "globals.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/llm_presets.hpp"
#include "../lib/util/llm_sessions.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include <progressive/llm.hpp>
#include <progressive/typing_indicator.hpp>
#include <progressive/typing_utils.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string_view>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace matrixcli;
using matrixcli::util::archiveSession;
using matrixcli::util::llmSessionPathFor;
using matrixcli::util::llmSessionsDir;

// Forward declarations for the commands defined in agent_commands_llm.cpp.
int cmdLlm(const cli::Args& args);
int cmdAgent(const cli::Args& args);


int cmdTyping(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli typing <room>" << std::endl;
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
        std::cerr << "Usage: progressive-cli agent-code <prompt> [--provider openai|anthropic]"
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
    // The config file first, the CLI flags win over it.
    agenttools::loadAgentConfig(cfg);
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
        [&](const std::string& cmd) -> int {
            std::cout << "run: " << cmd << " [y/N/a/A] " << std::flush;
            std::string ans;
            std::getline(std::cin, ans);
            if (ans == "y" || ans == "Y") return 1;
            if (ans == "a") return 2;
            if (ans == "A") return 3;
            return 0;
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
        },
        [](const std::string& t) { std::cout << t << std::flush; });
    if (!res.ok) {
        std::cerr << "agent error: " << res.error << std::endl;
        return 1;
    }
    if (!res.streamed) std::cout << res.text << std::endl;
    mkdir(".agent-sessions", 0755);
    saveSession(".agent-sessions/last.json", history);
    return 0;
}

void registerAgentCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("llm", cmdLlm, "LLM completion: llm <prompt> | llm chat [--fresh] [--rich] [--markdown] [--model m]");
    reg.registerCli("agent", cmdAgent, "Agentic loop with Matrix tools: agent <task> [--room X] [--provider p] [--model m]");
    reg.registerCli("agent-code", cmdAgentCode, "Local coding agent: agent-code <prompt> [--provider] [--model] [--trust allow|ask|deny]");
    reg.registerCli("typing", cmdTyping, "Who is typing: typing <room>");
}
