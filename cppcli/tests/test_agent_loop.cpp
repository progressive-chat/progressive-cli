// The agent-loop integration test: a fake OpenAI-compatible HTTP server
// drives the real agenttools::run engine — the tool calls (the shell)
// execute for real in a temp directory, the results feed back, the final
// answer lands. The same flow that broke in production repeatedly.
#include <cassert>
#include <cstdio>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); return 1; } \
} while (0)

#include "../src/agent_tools.hpp"

namespace matrixcli {
    std::atomic<bool> g_interrupted{true};
    std::atomic<bool> g_agentInterrupt{false};
}

namespace fs = std::filesystem;
using nlohmann::json;

// ---- the minimal fake OpenAI-compatible server ----

static std::atomic<bool> s_stop{false};
static int s_port = 0;

static void serverThread() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    assert(srv >= 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // The ephemeral port (0) does not resolve back via getsockname in
    // every environment — scan a small fixed range instead.
    int port = -1;
    for (int p = 18100; p < 18120 && port < 0; ++p) {
        addr.sin_port = htons(p);
        if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            port = p;
    }
    assert(port > 0);
    s_port = port;
    if (listen(srv, 4) != 0) return;

    while (!s_stop.load()) {
        // The accept with a poll timeout, so the shutdown can win.
        pollfd pfd{srv, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) continue;
        const int c = accept(srv, nullptr, nullptr);
        if (c < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED)
                continue;
            break;
        }

        // Read the headers, then the body (Content-Length).
        std::string req;
        char buf[4096];
        size_t contentLength = 0;
        size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos) {
            const ssize_t n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            headerEnd = req.find("\r\n\r\n");
        }
        if (headerEnd != std::string::npos) {
            const auto cl = req.find("Content-Length:");
            if (cl != std::string::npos) {
                const auto eol = req.find("\r\n", cl);
                contentLength = std::stoul(
                    req.substr(cl + 15, eol - cl - 15));
            }
            while (req.size() < headerEnd + 4 + contentLength) {
                const ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
        }

        json reqBody = json::object();
        const std::string rawBody =
            req.substr(headerEnd == std::string::npos ? 0 : headerEnd + 4,
                       contentLength);
        if (!rawBody.empty()) {
            try { reqBody = json::parse(rawBody); } catch (...) {}
        }

        json resp;
        bool hasToolResult = false;
        std::string lastToolResult;
        for (const auto& m : reqBody.value("messages", json::array())) {
            if (m.value("role", "") == "tool") {
                hasToolResult = true;
                lastToolResult = m.value("content", "");
            }
        }

        if (!hasToolResult) {
            // Step 1: ask for the shell tool.
            json toolCall = {{"id", "call_1"},
                             {"type", "function"},
                             {"function", {{"name", "shell"},
                                           {"arguments", "{\"command\":\"touch marker.txt\"}"}}}};
            json message = {{"role", "assistant"},
                            {"content", nullptr},
                            {"tool_calls", json::array({toolCall})}};
            resp = {{"choices", json::array({{{"message", message}}})},
                    {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}}};
        } else {
            // Step 2: the tool ran — the final answer.
            json message = {{"role", "assistant"},
                            {"content", "FINAL: marker created (" + lastToolResult + ")"}};
            resp = {{"choices", json::array({{{"message", message}}})},
                    {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 7}}}};
        }

        const std::string body = resp.dump();
        const std::string head =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        send(c, head.data(), head.size(), MSG_NOSIGNAL);
        send(c, body.data(), body.size(), MSG_NOSIGNAL);
        close(c);
    }
    close(srv);
}

int main() {
    const fs::path workdir = "/tmp/matrixcli_agent_loop_test";
    fs::remove_all(workdir);
    fs::create_directories(workdir);

    std::thread server(serverThread);
    while (s_port == 0) usleep(10000);

    matrixcli::agenttools::Config cfg;
    cfg.provider = "openai";
    cfg.endpoint = "http://127.0.0.1:" + std::to_string(s_port);
    cfg.model = "test-model";
    cfg.key = "x";
    cfg.trust = "allow";
    cfg.cwd = workdir.string();

    auto finish = [&](int rc) {
        s_stop = true;
        if (server.joinable()) server.join();
        fs::remove_all(workdir);
        return rc;
    };

    std::vector<matrixcli::agenttools::Message> history;
    std::fprintf(stderr, "[test] starting run...\n");
    matrixcli::agenttools::Result res =
        matrixcli::agenttools::run(cfg, "create marker.txt", history,
                                   nullptr, nullptr,
                                   [](const std::string& l) {
                                       std::fprintf(stderr, "[agent] %s\n", l.c_str());
                                   },
                                   nullptr);
    std::fprintf(stderr, "[test] run done ok=%d\n", res.ok);

    assert(res.ok);
    assert(res.text == "FINAL: marker created (");
    if (!res.ok) std::fprintf(stderr, "res.error=%s\n", res.error.c_str());
    CHECK(res.ok);
    CHECK(res.text.starts_with("FINAL: marker created"));
    CHECK(fs::exists(workdir / "marker.txt"));
    // The conversation recorded the tool call + the result.
    bool sawTool = false;
    for (const auto& m : history) {
        if (m.role == "tool") sawTool = true;
    }
    CHECK(sawTool);

    s_stop = true;
    server.join();
    fs::remove_all(workdir);
    std::printf("test_agent_loop: all ok\n");
    return 0;
}
