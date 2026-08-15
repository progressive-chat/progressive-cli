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
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include <progressive/llm.hpp>
#include <progressive/typing_indicator.hpp>
#include <progressive/typing_utils.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace matrixcli;

// Build the agent config from the CLI args. The flags win over
// agent.json / the env / the provider defaults (see applyDefaults).
static matrixagent::Config matrixAgentConfigFromArgs(const cli::Args& args) {
    matrixagent::Config cfg;
    if (args.options.count("provider")) cfg.provider = args.options.at("provider");
    if (args.options.count("endpoint")) cfg.endpoint = args.options.at("endpoint");
    if (args.options.count("model")) cfg.model = args.options.at("model");
    if (args.options.count("token")) cfg.key = args.options.at("token");
    if (args.options.count("proxy")) cfg.proxy = args.options.at("proxy");
    if (args.options.count("system")) cfg.extraSystem = args.options.at("system");
    if (args.options.count("max-tokens")) {
        try { cfg.maxTokens = std::stoi(args.options.at("max-tokens")); } catch (...) {}
    }
    if (args.options.count("max-iterations")) {
        try { cfg.maxIterations = std::stoi(args.options.at("max-iterations")); } catch (...) {}
    }
    if (args.options.count("temperature")) {
        try { cfg.temperature = std::stod(args.options.at("temperature")); } catch (...) {}
    }
    if (args.options.count("top-p")) {
        try { cfg.topP = std::stod(args.options.at("top-p")); } catch (...) {}
    }
    if (args.options.count("reasoning")) {
        std::string r = args.options.at("reasoning");
        if (r == "low" || r == "medium" || r == "high") cfg.reasoning = r;
    }
    if (args.options.count("debug-llm")) cfg.debugLlm = true;
    cfg.verbose = args.options.count("verbose");
    matrixagent::applyDefaults(cfg);
    return cfg;
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

// The metadata block (the rich style): the model, the request time, the
// token counts, the estimated price and the context usage. Goes to stderr
// so the piped stdout keeps the bare answer.
static void printMeta(const matrixagent::Completion& c) {
    char tbuf[16];
    time_t t = static_cast<time_t>(c.ts / 1000);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&t));
    std::ostringstream meta;
    meta << "\x1b[90m[" << c.model << " · " << tbuf;
    int64_t total = c.promptTokens + c.completionTokens;
    if (total > 0) {
        meta << " · tok " << c.promptTokens << "/" << c.completionTokens
             << " (" << total << ")";
        double cost = agenttools::estimateCost(c.model, c.promptTokens, c.completionTokens);
        if (cost > 0) {
            char cbuf[24];
            snprintf(cbuf, sizeof(cbuf), "$%.5f", cost);
            meta << " · " << cbuf;
        }
        int ctx = agenttools::contextSizeForModel(c.model);
        if (ctx > 0 && c.promptTokens > 0) {
            double pct = static_cast<double>(c.promptTokens) * 100.0 / ctx;
            char pbuf[24];
            snprintf(pbuf, sizeof(pbuf), "%.2f", pct);
            meta << " · ctx " << pbuf << "%";
        }
    }
    meta << "]\x1b[0m";
    std::cerr << meta.str() << std::endl;
}

static int cmdLlmChat(const cli::Args& args);
static int cmdLlmModels(const cli::Args& args);
static int cmdLlmHistory(const cli::Args& args);
static int cmdLlmSessions(const cli::Args& args);
static int cmdLlmResume(const cli::Args& args);

// The conversation store: ~/.local/share/matrixcli/sessions/llm-<name>.json
// (XDG_DATA_HOME when set). The sessions live OUTSIDE the working
// directory, so they survive running the client from another directory.
// On load, the legacy ./agent-sessions/llm-*.json files are still found
// (one-time fallback).
static std::string llmSessionsDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg && *xdg ? std::string(xdg)
                                   : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.local/share";
    return base + "/matrixcli/sessions";
}

static std::string llmSessionPathFor(const std::string& name) {
    if (name.empty()) return llmSessionsDir() + "/llm-chat.json";
    std::string clean;
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') clean += (char)ch;
        else clean += '_';
    }
    return llmSessionsDir() + "/llm-" + clean + ".json";
}

// The active session name for these args ("" = the default conversation).
static std::string llmSessionName(const cli::Args& args) {
    return args.options.count("session") ? args.options.at("session") : "";
}

// The --fresh start: the existing conversation is ARCHIVED (renamed with
// a timestamp) instead of destroyed — `llm sessions` then lists every
// dialog ever started.
static void archiveSession(const std::string& name) {
    std::string path = llmSessionPathFor(name);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return;
    std::filesystem::create_directories(llmSessionsDir());
    std::string stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string base = name.empty() ? std::string("chat") : name;
    ::rename(path.c_str(), (llmSessionsDir() + "/llm-" + base + "-" + stamp
                            + ".json").c_str());
}

// The presentation: the CLI flags win over agent.json (llm_style /
// llm_markdown), the default is the plain answer.
static void loadPresentation(const cli::Args& args, bool& rich, bool& markdown) {
    rich = false;
    markdown = false;
    agenttools::Config file;
    if (agenttools::loadAgentConfig(file)) {
        rich = file.llmStyle == "rich";
        markdown = file.llmMarkdown;
    }
    if (args.options.count("rich")) rich = true;
    if (args.options.count("style")) rich = args.options.at("style") == "rich";
    if (args.options.count("simple")) rich = false;
    if (args.options.count("markdown")) markdown = true;
    if (args.options.count("no-markdown")) markdown = false;
}

// The streaming policy: --stream / --no-stream win, otherwise the output
// streams when stdout is a terminal. --json and --markdown need the whole
// response, so they force the non-streaming path.
static bool wantStream(const cli::Args& args) {
    if (args.options.count("json")) return false;
    if (args.options.count("markdown")) return false;
    if (args.options.count("stream")) return true;
    if (args.options.count("no-stream")) return false;
    return isatty(STDOUT_FILENO);
}

// The reasoning block — light grey, the "[thinking]" label (shown by
// default; --no-reasoning hides it).
static void printReasoning(const matrixagent::Completion& c) {
    if (c.reasoning.empty()) return;
    std::cout << "\x1b[38;5;245m[thinking]\n" << c.reasoning << "\x1b[0m\n";
}

// The auto-compaction: when the conversation estimate passes 60% of the
// model's context window, the old turns are summarized by the LLM into
// one context message (the last 4 messages stay verbatim).
static std::vector<agenttools::Message> compactHistory(
        const matrixagent::Config& cfg, const std::string& system,
        const std::vector<agenttools::Message>& hist) {
    if (hist.size() <= 6) return hist;
    int64_t est = 0;
    for (const auto& m : hist) est += m.content.size() / 4 + 4;
    int ctx = agenttools::contextSizeForModel(cfg.model);
    if (ctx <= 0 || est * 100 / ctx < 60) return hist;

    size_t tailN = std::min<size_t>(4, hist.size());
    std::vector<agenttools::Message> tail(hist.end() - tailN, hist.end());
    std::vector<agenttools::Message> head(hist.begin(), hist.end() - tailN);

    std::vector<matrixagent::ChatMessage> msgs;
    for (const auto& m : head) {
        if (m.role == "user" || m.role == "assistant")
            msgs.push_back({m.role, m.content});
    }
    msgs.push_back({"user",
        "Summarize this conversation into compact bullet points: the facts, "
        "the decisions and the current task state. Under 500 words. Return "
        "ONLY the summary."});
    std::string err;
    matrixagent::Completion c = matrixagent::chat(cfg, system, msgs, err);
    if (!c.ok) return hist;

    std::vector<agenttools::Message> out;
    out.push_back({"user", "[earlier conversation summary]\n" + c.text, {}, "", ""});
    for (const auto& m : tail) out.push_back(m);
    return out;
}

// One completion with the user's presentation (streaming + rich meta +
// the usage accounting). Returns the Completion (ok=false on failure).
static matrixagent::Completion runOneCompletion(const matrixagent::Config& cfg,
                            const std::string& system,
                            const std::vector<matrixagent::ChatMessage>& msgs,
                            bool rich, bool markdown, bool stream,
                            bool json_out, bool showReasoning) {
    matrixagent::Completion c;
    if (stream) {
        c = matrixagent::stream(
            cfg, system, msgs,
            [](const std::string& piece) { std::cout << piece << std::flush; },
            showReasoning
                ? [](const std::string& rp) {
                      std::cout << "\x1b[38;5;245m" << rp << "\x1b[0m" << std::flush;
                  }
                : std::function<void(const std::string&)>{});
        if (c.ok) std::cout << std::endl;
    } else {
        std::string err;
        c = matrixagent::chat(cfg, system, msgs, err);
    }
    if (!c.ok) {
        std::cerr << "LLM error: " << c.error << std::endl;
        return c;
    }
    agenttools::agentAddUsage(c.promptTokens, c.completionTokens, c.model);
    if (json_out) {
        nlohmann::json j;
        j["ok"] = true;
        j["text"] = c.text;
        j["error"] = "";
        j["model"] = c.model;
        j["ts"] = c.ts;
        j["reasoning"] = c.reasoning;
        j["tokens"] = {{"prompt", c.promptTokens},
                       {"completion", c.completionTokens},
                       {"total", c.promptTokens + c.completionTokens}};
        j["price_usd"] = agenttools::estimateCost(c.model, c.promptTokens, c.completionTokens);
        j["context_window"] = agenttools::contextSizeForModel(c.model);
        std::cout << j.dump() << std::endl;
    } else {
        if (rich) printMeta(c);
        if (showReasoning && !stream) printReasoning(c);
        if (!stream) {
            std::cout << (markdown ? renderMarkdownAnsi(c.text) : c.text)
                      << std::endl;
        }
    }
    return c;
}

// The --tools turn: the same prompt, but the model can CALL tools (the
// agent-code engine: filesystem + shell with the trust policy, subagents,
// MCP, streaming). The conversation persists in the same session store.
static int runLlmTurnTools(const cli::Args& args, const std::string& prompt) {
    agenttools::Config cfg;
    agenttools::loadAgentConfig(cfg);  // the trust/allow/deny/mcp rules
    // The provider/model/key/proxy from the unified config (the flags win).
    matrixagent::Config mcfg = matrixAgentConfigFromArgs(args);
    cfg.provider = mcfg.provider;
    cfg.endpoint = mcfg.endpoint;
    cfg.model = mcfg.model;
    cfg.key = mcfg.key;
    cfg.proxy = mcfg.proxy;
    if (args.options.count("trust")) cfg.trust = args.options.at("trust");
    if (args.options.count("max-iterations")) {
        try { cfg.maxIterations = std::stoi(args.options.at("max-iterations")); } catch (...) {}
    }
    char cwdbuf[4096];
    if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;

    bool json_out = args.options.count("json");
    bool rich = false;
    bool markdown = false;
    loadPresentation(args, rich, markdown);
    bool stream = wantStream(args) && !json_out;

    std::vector<agenttools::Message> hist;
    if (args.options.count("fresh")) archiveSession(llmSessionName(args));
    else agenttools::loadSession(llmSessionPathFor(llmSessionName(args)), hist);

    auto confirm = [](const std::string& cmd) -> int {
        std::cout << "run: " << cmd << " [y/N/a/A] " << std::flush;
        std::string ans;
        std::getline(std::cin, ans);
        if (ans == "y" || ans == "Y") return 1;
        if (ans == "a") return 2;
        if (ans == "A") return 3;
        return 0;
    };
    auto ask = [](const std::string& questionsJson) -> std::string {
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
    };
    auto log = [&](const std::string& l) {
        if (args.options.count("verbose")) std::cerr << l << std::endl;
    };
    auto onToken = stream
        ? std::function<void(const std::string&)>(
              [](const std::string& t) { std::cout << t << std::flush; })
        : std::function<void(const std::string&)>{};

    agenttools::Result res = agenttools::run(cfg, prompt, hist, confirm, ask,
                                             log, onToken);
    if (!res.ok) {
        std::cerr << "llm error: " << res.error << std::endl;
        return 1;
    }
    std::filesystem::create_directories(llmSessionsDir());
    agenttools::saveSession(llmSessionPathFor(llmSessionName(args)), hist);

    if (json_out) {
        nlohmann::json j;
        j["ok"] = true;
        j["text"] = res.text;
        j["error"] = "";
        j["model"] = cfg.model;
        j["iterations"] = res.iterations;
        j["tokens"] = {{"prompt", res.promptTokens},
                       {"completion", res.completionTokens},
                       {"total", res.promptTokens + res.completionTokens}};
        j["reasoning"] = res.reasoning;
        std::cout << j.dump() << std::endl;
    } else {
        // The full presentation: the grey thinking block, the rich meta
        // (model/time/tokens/price/context) and the answer.
        bool showReasoning = !args.options.count("no-reasoning");
        matrixagent::Completion c;
        c.ok = true;
        c.text = res.text;
        c.model = cfg.model;
        c.promptTokens = res.promptTokens;
        c.completionTokens = res.completionTokens;
        c.reasoning = res.reasoning;
        c.ts = res.ts;
        if (rich) printMeta(c);
        if (showReasoning && !res.reasoning.empty()) printReasoning(c);
        if (!res.streamed) {
            std::cout << (markdown ? renderMarkdownAnsi(res.text) : res.text)
                      << std::endl;
        } else {
            std::cout << std::endl;
        }
    }
    return 0;
}

// One turn on the saved conversation: load (archive on --fresh), send the
// prompt with the full history, save. Returns the exit code.
static int runLlmTurn(const cli::Args& args, const std::string& prompt) {
    if (args.options.count("tools")) return runLlmTurnTools(args, prompt);
    auto cfg = matrixAgentConfigFromArgs(args);
    if (cfg.key.empty()) {
        std::cerr << "Error: no API key — run './matrixcli tui agent' for the interactive setup, or set it in ~/.config/matrixcli/agent.json, export OPENAI_API_KEY/ANTHROPIC_API_KEY, or pass --token" << std::endl;
        return 1;
    }
    bool json_out = args.options.count("json");
    bool rich = false;
    bool markdown = false;
    loadPresentation(args, rich, markdown);
    bool showReasoning = !args.options.count("no-reasoning");  // shown by default
    std::string systemPrompt = args.options.count("system")
                                   ? args.options.at("system") : cfg.extraSystem;

    std::vector<agenttools::Message> hist;
    if (args.options.count("fresh")) {
        archiveSession(llmSessionName(args));  // never destroy, archive
    } else {
        agenttools::loadSession(llmSessionPathFor(llmSessionName(args)), hist);
    }
    hist.push_back({"user", prompt, {}, "", ""});
    // The auto-compaction keeps the long conversations inside the context
    // (the compacted history is persisted back).
    if (hist.size() > 6) {
        std::vector<agenttools::Message> compacted =
            compactHistory(cfg, systemPrompt, hist);
        if (compacted.size() < hist.size()) hist = compacted;
    }
    std::vector<matrixagent::ChatMessage> msgs;
    for (const auto& m : hist) {
        if (m.role == "user" || m.role == "assistant")
            msgs.push_back({m.role, m.content});
    }
    matrixagent::Completion c = runOneCompletion(
        cfg, systemPrompt, msgs, rich, markdown, wantStream(args), json_out,
        showReasoning);
    if (!c.ok) return 1;
    hist.push_back({"assistant", c.text, {}, "", ""});
    std::filesystem::create_directories(llmSessionsDir());
    agenttools::saveSession(llmSessionPathFor(llmSessionName(args)), hist);
    return 0;
}

int cmdLlm(const cli::Args& args) {
    // The subcommands: chat (interactive), models (the presets), history,
    // sessions (the saved conversations) and resume (switch by number).
    if (!args.positional.empty()) {
        if (args.positional[0] == "chat") return cmdLlmChat(args);
        if (args.positional[0] == "models") return cmdLlmModels(args);
        if (args.positional[0] == "history") return cmdLlmHistory(args);
        if (args.positional[0] == "sessions") return cmdLlmSessions(args);
        if (args.positional[0] == "resume") return cmdLlmResume(args);
    }

    // One-shot and continue are the SAME flow on the saved conversation:
    //   llm "q1"              → turn 1, saved to .agent-sessions/llm-chat.json
    //   llm continue "q2"     → turn 2 (the full history is sent), saved
    //   llm --fresh "q1"      → archive + start a new conversation
    //   llm --session work …  → a named conversation (llm-work.json)
    //   llm chat              → continue it interactively
    // The only difference: `continue` also accepts the message on stdin.
    bool isContinue = !args.positional.empty() && args.positional[0] == "continue";

    std::string prompt;
    size_t from = isContinue ? 1 : 0;
    for (size_t i = from; i < args.positional.size(); i++) {
        if (!prompt.empty()) prompt += " ";
        prompt += args.positional[i];
    }
    if (prompt.empty() && !isatty(STDIN_FILENO)) {
        std::string l;
        while (std::getline(std::cin, l)) {
            if (!prompt.empty()) prompt += "\n";
            prompt += l;
        }
    }
    if (prompt.empty()) {
        std::cerr << "Usage: matrixcli llm <prompt> | llm continue <message> | llm chat"
                     " | llm resume <N> [message] | llm models | llm history | llm sessions"
                     " [--session <name>] [--fresh] [--system s]"
                     " [--provider openai|anthropic|deepseek|mimo...] [--token t]"
                     " [--endpoint URL] [--model m] [--max-tokens n]"
                     " [--temperature x] [--top-p x] [--reasoning low|medium|high]"
                     " [--tools] [--trust allow|ask|deny] [--no-reasoning] [--proxy host:port] [--stream|--no-stream]"
                     " [--rich|--style rich|simple] [--markdown|--no-markdown] [--json]" << std::endl;
        return 1;
    }
    return runLlmTurn(args, prompt);
}


int cmdLlmChat(const cli::Args& args) {
    auto cfg = matrixAgentConfigFromArgs(args);
    if (cfg.key.empty()) {
        std::cerr << "Error: no API key — run './matrixcli tui agent' for the interactive setup, or set it in ~/.config/matrixcli/agent.json, export OPENAI_API_KEY/ANTHROPIC_API_KEY, or pass --token" << std::endl;
        return 1;
    }
    bool rich = false;
    bool markdown = false;
    loadPresentation(args, rich, markdown);
    bool showReasoning = !args.options.count("no-reasoning");  // shown by default
    std::string systemPrompt = args.options.count("system")
                                   ? args.options.at("system") : cfg.extraSystem;

    // The conversation persists across runs (.agent-sessions/llm-chat.json
    // or llm-<session>.json); --fresh archives the old one and starts new.
    std::vector<agenttools::Message> hist;
    if (args.options.count("fresh")) {
        archiveSession(llmSessionName(args));  // never destroy, archive
    } else {
        agenttools::loadSession(llmSessionPathFor(llmSessionName(args)), hist);
    }

    std::cout << "llm chat (" << cfg.model << ")" << llmSessionName(args)
              << " — /clear, /exit; Ctrl+C leaves."
              << (hist.empty() ? "" : " — " + std::to_string(hist.size() / 2) + " turns restored")
              << std::endl;

    std::vector<std::string> inputHistory;
    for (;;) {
        std::string line;
        if (!readLineWithHistory(inputHistory, "you> ", line)) break;  // Ctrl+C
        if (line == "/exit" || line == "/quit" || line == "/q") break;
        if (line == "/clear") {
            hist.clear();
            std::cout << "(history cleared)" << std::endl;
            continue;
        }
        if (line.empty()) continue;
        if (args.options.count("tools")) {
            // The --tools chat: each turn can call the tools (the same
            // engine as the one-shot --tools path).
            agenttools::Config cfg;
            agenttools::loadAgentConfig(cfg);
            matrixagent::Config mcfg = matrixAgentConfigFromArgs(args);
            cfg.provider = mcfg.provider;
            cfg.endpoint = mcfg.endpoint;
            cfg.model = mcfg.model;
            cfg.key = mcfg.key;
            cfg.proxy = mcfg.proxy;
            if (args.options.count("trust")) cfg.trust = args.options.at("trust");
            char cwdbuf[4096];
            if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;
            auto confirm = [](const std::string& cmd) -> int {
                std::cout << "run: " << cmd << " [y/N/a/A] " << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                if (ans == "y" || ans == "Y") return 1;
                if (ans == "a") return 2;
                if (ans == "A") return 3;
                return 0;
            };
            auto ask = [](const std::string& questionsJson) -> std::string {
                nlohmann::json qs;
                try { qs = nlohmann::json::parse(questionsJson); }
                catch (...) { return "error: bad questions JSON"; }
                std::string out;
                for (const auto& q : qs.value("questions", nlohmann::json::array())) {
                    std::cout << "  Q: " << q.value("question", "?") << std::endl;
                    auto opts = q.value("options", nlohmann::json::array());
                    for (size_t i = 0; i < opts.size(); ++i)
                        std::cout << "    " << i + 1 << ") "
                                  << opts[i].value("label", "") << std::endl;
                    std::cout << "  answer> " << std::flush;
                    std::string ans;
                    std::getline(std::cin, ans);
                    out += "Q: " + q.value("question", "?") + "\nA: " + ans + "\n";
                }
                return out.empty() ? "(no questions answered)" : out;
            };
            agenttools::Result res = agenttools::run(
                cfg, line, hist,
                confirm, ask,
                [](const std::string&) {},
                wantStream(args)
                    ? std::function<void(const std::string&)>(
                          [](const std::string& t) { std::cout << t << std::flush; })
                    : std::function<void(const std::string&)>{});
            if (!res.ok) {
                std::cerr << "llm error: " << res.error << std::endl;
                continue;
            }
            if (!res.streamed) std::cout << res.text << std::endl;
            else std::cout << std::endl;
            continue;
        }
        hist.push_back({"user", line, {}, "", ""});
        // The auto-compaction keeps the long conversations in the context.
        if (hist.size() > 6) {
            std::vector<agenttools::Message> compacted =
                compactHistory(cfg, systemPrompt, hist);
            if (compacted.size() < hist.size()) {
                hist = compacted;
                std::cout << "\x1b[90m(history compacted)\x1b[0m" << std::endl;
            }
        }
        std::vector<matrixagent::ChatMessage> msgs;
        for (const auto& m : hist) {
            if (m.role == "user" || m.role == "assistant")
                msgs.push_back({m.role, m.content});
        }
        matrixagent::Completion c = runOneCompletion(
            cfg, systemPrompt, msgs, rich, markdown, wantStream(args), false,
            showReasoning);
        if (!c.ok) {
            hist.pop_back();
            continue;
        }
        hist.push_back({"assistant", c.text, {}, "", ""});
    }
    std::filesystem::create_directories(llmSessionsDir());
    agenttools::saveSession(llmSessionPathFor(llmSessionName(args)), hist);
    std::cout << "(saved " << hist.size() / 2 << " turns)" << std::endl;
    return 0;
}

// llm models — the provider presets + the current model.
int cmdLlmModels(const cli::Args& args) {
    matrixagent::Config cfg = matrixAgentConfigFromArgs(args);
    std::cout << "Provider presets:" << std::endl;
    for (const auto& p : util::llmPresets()) {
        bool current = cfg.endpoint == p.endpoint && cfg.model == p.model;
        std::cout << (current ? " * " : "   ") << p.name
                  << "  (" << p.model << ")";
        if (p.local) std::cout << "  [local, no key]";
        if (current) std::cout << "  ← current";
        std::cout << std::endl;
    }
    std::cout << "\ncurrent: " << cfg.model << "\n"
              << "endpoint: " << cfg.endpoint << "\n"
              << "the list: ~/.config/matrixcli/agent.json (agent config provider <name>)"
              << std::endl;
    return 0;
}

// llm history — the saved conversation (the named session via --session).
int cmdLlmHistory(const cli::Args& args) {
    std::vector<agenttools::Message> hist;
    if (!agenttools::loadSession(llmSessionPathFor(llmSessionName(args)), hist)) {
        std::cerr << "No saved session" << llmSessionName(args)
                  << " (run llm <prompt> first)" << std::endl;
        return 1;
    }
    int turns = 0;
    for (const auto& m : hist) {
        if (m.role == "user") {
            std::cout << "\x1b[1myou>\x1b[0m " << m.content << "\n\n";
        } else if (m.role == "assistant") {
            std::cout << m.content << "\n\x1b[90m────\x1b[0m\n";
            turns++;
        }
    }
    if (!turns) std::cout << "(empty session)" << std::endl;
    return 0;
}

// The saved-conversation listing shared by `sessions` and `resume`:
// numbered by recency, with the previews and the file paths.
struct LlmSessionEntry {
    std::string display;   // "chat" / "work" / "chat · 2026-08-15 14:43"
    std::string rawName;   // the file-derived name (for --session)
    std::string path;      // the full file path
    int turns = 0;
    std::string firstUser;  // the topic (the first user message)
    std::string lastMsg;    // the last exchange
    long mtime = 0;
};

static std::vector<LlmSessionEntry> collectLlmSessions() {
    namespace fs = std::filesystem;
    std::vector<LlmSessionEntry> entries;
    std::vector<std::string> dirs = {llmSessionsDir()};
    // The legacy location (the working directory) — still listed for the
    // one-time migration.
    dirs.push_back(".agent-sessions");
    for (const auto& dir : dirs) {
        if (!fs::exists(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir)) {
        std::string fn = e.path().filename().string();
        if (fn.rfind("llm-", 0) != 0 || fn.find(".json") == std::string::npos)
            continue;
        std::string rawName = fn.substr(4);
        rawName = rawName.substr(0, rawName.size() - 5);
        if (rawName.empty()) rawName = "chat";
        // The archived sessions: "chat-<epoch>" → "chat · 2026-08-15 14:43".
        std::string display = rawName;
        auto dash = display.find_last_of('-');
        if (dash != std::string::npos) {
            std::string tail = display.substr(dash + 1);
            bool digits = !tail.empty() &&
                          std::all_of(tail.begin(), tail.end(),
                                      [](unsigned char c){ return std::isdigit(c); });
            if (digits && tail.size() >= 10) {
                std::time_t t = static_cast<std::time_t>(std::stoll(tail));
                char buf[32];
                std::tm tm{};
                localtime_r(&t, &tm);
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min);
                display = display.substr(0, dash) + " · " + buf;
            }
        }
        std::vector<agenttools::Message> hist;
        int turns = 0;
        std::string firstUser, lastMsg;
        if (agenttools::loadSession(e.path().string(), hist)) {
            for (const auto& m : hist) {
                if (m.role == "assistant") turns++;
                if (firstUser.empty() && m.role == "user") firstUser = m.content;
                if (m.role == "user" || m.role == "assistant") lastMsg = m.content;
            }
        }
        auto mt = fs::last_write_time(e);
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            mt.time_since_epoch()).count();
            entries.push_back({display, rawName, e.path().string(), turns,
                               firstUser, lastMsg, static_cast<long>(secs)});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const LlmSessionEntry& a, const LlmSessionEntry& b) {
                  return a.mtime > b.mtime;
              });
    return entries;
}

// llm sessions — the saved conversations: list or remove.
int cmdLlmSessions(const cli::Args& args) {
    // matrixcli llm sessions [rm <name>] — the subcommand args sit in
    // positional[1:] ("sessions" itself is positional[0]).
    if (args.positional.size() >= 2 && args.positional[1] == "rm") {
        if (args.positional.size() < 3) {
            std::cerr << "Usage: matrixcli llm sessions rm <name>" << std::endl;
            return 1;
        }
        std::string path = llmSessionPathFor(args.positional[2]);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            std::cerr << "No such session: " << args.positional[2] << std::endl;
            return 1;
        }
        // Never destroy: the session moves to the trash subdirectory
        // (recoverable by hand, or by --session with the trashed name).
        std::filesystem::create_directories(llmSessionsDir() + "/trash");
        std::string stamp = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        std::string to = llmSessionsDir() + "/trash/llm-" + args.positional[2]
                       + "-" + stamp + ".json";
        if (::rename(path.c_str(), to.c_str()) == 0) {
            std::cout << "Moved " << args.positional[2]
                      << " to the trash (nothing is ever deleted)." << std::endl;
        } else {
            std::cerr << "Failed to move " << args.positional[2] << std::endl;
            return 1;
        }
        return 0;
    }
    auto clipLine = [](std::string s, size_t n) -> std::string {
        for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
        if (s.size() > n) s = s.substr(0, n - 1) + "…";
        return s;
    };
    auto entries = collectLlmSessions();
    std::cout << "Saved conversations:" << std::endl;
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& en = entries[i];
        std::cout << "  " << (i + 1) << ". " << en.display
                  << (en.rawName == "chat" ? " (active)" : "")
                  << " — " << en.turns << " turns" << std::endl;
        if (!en.firstUser.empty()) {
            std::cout << "      \x1b[90m» " << clipLine(en.firstUser, 64)
                      << "\x1b[0m" << std::endl;
        }
        if (!en.lastMsg.empty()) {
            std::cout << "      \x1b[90m… " << clipLine(en.lastMsg, 64)
                      << "\x1b[0m" << std::endl;
        }
    }
    if (entries.empty())
        std::cout << "  (none — run llm <prompt> or llm chat first)" << std::endl;
    std::cout << "\nresume: llm resume <N> [\"message\"]          — switch + continue by number\n"
              << "        llm continue \"...\"                    — the chat session\n"
              << "        llm --session <name> continue \"...\" — a named/archived session\n"
              << "        llm chat [--session <name>]            — interactively\n"
              << "remove: matrixcli llm sessions rm <name>" << std::endl;
    return 0;
}

// llm resume <N> ["message"] — switch the active chat to the session N
// (the old chat is archived, never destroyed); with a message, the turn
// is sent immediately.
int cmdLlmResume(const cli::Args& args) {
    namespace fs = std::filesystem;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: matrixcli llm resume <N> [\"message\"] "
                     "(the numbers come from 'llm sessions')" << std::endl;
        return 1;
    }
    int n = 0;
    try { n = std::stoi(args.positional[1]); } catch (...) { n = 0; }
    auto entries = collectLlmSessions();
    if (n < 1 || n > static_cast<int>(entries.size())) {
        std::cerr << "No session #" << args.positional[1]
                  << " — run 'matrixcli llm sessions' for the list." << std::endl;
        return 1;
    }
    const auto& target = entries[n - 1];
    std::string activePath = llmSessionPathFor("");
    if (target.path != activePath) {
        archiveSession("");  // the old active chat is archived
        fs::copy_file(target.path, activePath,
                      fs::copy_options::overwrite_existing);
        std::cout << "Resumed session " << n << " (" << target.display
                  << ", " << target.turns << " turns)." << std::endl;
    } else {
        std::cout << "Session " << n << " (" << target.display
                  << ") is already the active chat." << std::endl;
    }

    std::string prompt;
    for (size_t i = 2; i < args.positional.size(); i++) {
        if (!prompt.empty()) prompt += " ";
        prompt += args.positional[i];
    }
    if (prompt.empty()) {
        std::cout << "Continue with: matrixcli llm continue \"...\"" << std::endl;
        return 0;
    }
    return runLlmTurn(args, prompt);
}

int cmdAgent(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    bool json_out = args.options.count("json");
    std::string task = readPrompt(args);
    if (task.empty()) {
        std::cerr << "Usage: matrixcli agent <task> [--room <id|name>] [--provider openai|anthropic|deepseek|mimo...] [--token t] [--endpoint URL] [--model m] [--proxy host:port] [--max-iterations n] [--json] [--verbose]" << std::endl;
        return 1;
    }
    auto cfg = matrixAgentConfigFromArgs(args);
    if (cfg.key.empty()) {
        std::cerr << "Error: no API key — run './matrixcli tui agent' for the interactive setup, or set it in ~/.config/matrixcli/agent.json, export OPENAI_API_KEY/ANTHROPIC_API_KEY, or pass --token" << std::endl;
        return 1;
    }
    std::string roomId = args.options.count("room") ? args.options.at("room") : "";

    auto backend = matrixagent::makeEcoreBackend();
    matrixagent::Result res = matrixagent::run(
        cfg, *backend, task, roomId,
        [&](const std::string& l) {
            if (cfg.verbose) std::cerr << l << std::endl;
        });

    if (json_out) {
        nlohmann::json j;
        j["ok"] = res.ok;
        j["answer"] = res.answer;
        j["iterations"] = res.iterations;
        j["error"] = res.error;
        std::cout << j.dump() << std::endl;
    } else if (!res.ok) {
        std::cerr << "Agent error: " << res.error << std::endl;
        return 1;
    } else if (!res.answer.empty()) {
        std::cout << res.answer << std::endl;
    } else {
        std::cout << "Agent finished without a final answer." << std::endl;
    }
    return res.ok ? 0 : 1;
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
