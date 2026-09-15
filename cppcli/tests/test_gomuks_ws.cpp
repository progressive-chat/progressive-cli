// test_gomuks_ws.cpp — M1 conformance for the gomuks backend client.
//
// A fake gomuks backend (loopback TCP, real RFC 6455 handshake + frames)
// drives WsClient/GomuksClient through: upgrade with cookie auth, server
// ping auto-pong, fragmented reassembly, get_state, ping/pong, error
// replies and unmatched-event dispatch. No Go toolchain needed.
#include "../lib/gomuks/gomuks_client.hpp"
#include "../lib/gomuks/jsoncmd.hpp"
#include "../lib/gomuks/ws_client.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <future>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <openssl/sha.h>
#include <nlohmann/json.hpp>

using matrixcli::gomuks::GomuksClient;
using matrixcli::gomuks::JsonEnvelope;
using matrixcli::gomuks::WsEndpoint;
using matrixcli::gomuks::base64Encode;
using matrixcli::gomuks::formatCommand;
using matrixcli::gomuks::parseEnvelope;
using matrixcli::gomuks::parseWsUrl;

static int failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "FAIL " << __LINE__ << ": " << #cond << std::endl;   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

namespace {

// --- Minimal blocking socket helpers for the fake server ---

bool writeAll(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t r = ::send(fd, s.data() + done, s.size() - done, 0);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

bool readUntil(int fd, const std::string& marker, std::string& out) {
    out.clear();
    char buf[1024];
    while (out.find(marker) == std::string::npos) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) return false;
        out.append(buf, static_cast<size_t>(r));
        if (out.size() > 65536) return false;
    }
    return true;
}

bool readN(int fd, char* dst, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = ::recv(fd, dst + done, n - done, 0);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

struct Frame {
    uint8_t opcode = 0;
    std::string payload;
    bool masked = false;
};

// Read one client frame and unmask it (the fake server is strict: client
// frames must be masked, like a real RFC 6455 server demands).
bool readClientFrame(int fd, Frame& f) {
    char h[2];
    if (!readN(fd, h, 2)) return false;
    uint8_t b0 = h[0], b1 = h[1];
    if (!(b0 & 0x80)) return false; // no fragmentation from our client
    f.opcode = b0 & 0x0F;
    f.masked = (b1 & 0x80) != 0;
    if (!f.masked) return false;
    uint64_t len = b1 & 0x7F;
    if (len == 126) {
        char e[2];
        if (!readN(fd, e, 2)) return false;
        len = (static_cast<uint64_t>(uint8_t(e[0])) << 8) | uint8_t(e[1]);
    } else if (len == 127) {
        char e[8];
        if (!readN(fd, e, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(e[i]);
    }
    if (len > 1024 * 1024) return false;
    char mask[4];
    if (!readN(fd, mask, 4)) return false;
    f.payload.assign(len, '\0');
    if (len && !readN(fd, &f.payload[0], (size_t)len)) return false;
    for (size_t i = 0; i < f.payload.size(); ++i)
        f.payload[i] ^= mask[i % 4];
    return true;
}

// Send one unmasked server frame (servers must not mask).
bool sendServerFrame(int fd, uint8_t opcode, const std::string& payload,
                     bool fin = true) {
    std::string f;
    f.push_back((char)((fin ? 0x80 : 0x00) | opcode));
    if (payload.size() < 126) f.push_back((char)payload.size());
    else if (payload.size() < 65536) {
        f.push_back((char)126);
        f.push_back((char)((payload.size() >> 8) & 0xFF));
        f.push_back((char)(payload.size() & 0xFF));
    } else return false;
    f += payload;
    return writeAll(fd, f);
}

std::string wsAccept(const std::string& key) {
    std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)material.data(), material.size(), digest);
    return base64Encode(std::string((char*)digest, sizeof(digest)));
}

std::string headerValue(const std::string& req, const std::string& name) {
    std::string needle = "\r\n" + name + ":";
    auto p = req.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    auto e = req.find("\r\n", p);
    std::string v = req.substr(p, e - p);
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    return v;
}

// The scripted fake backend. Reports failures through the shared counter.
void fakeBackend(std::promise<int> portPromise) {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CHECK(false);
        return;
    }
    if (listen(listenFd, 1) != 0) {
        CHECK(false);
        return;
    }
    socklen_t len = sizeof(addr);
    getsockname(listenFd, (sockaddr*)&addr, &len);
    portPromise.set_value(ntohs(addr.sin_port));
    int fd = accept(listenFd, nullptr, nullptr);
    close(listenFd);
    if (fd < 0) {
        CHECK(false);
        return;
    }

    std::string req;
    CHECK(readUntil(fd, "\r\n\r\n", req));
    CHECK(req.find("Upgrade: websocket") != std::string::npos);
    std::string key = headerValue(req, "Sec-WebSocket-Key");
    CHECK(!key.empty());
    CHECK(headerValue(req, "Cookie") == "gomuks_auth=test-cookie");
    CHECK(writeAll(fd, "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " +
                           wsAccept(key) + "\r\n\r\n"));

    // 1. Server ping -> client must auto-reply pong with same payload.
    // NOTE: a correct client pipelines its get_state request before ever
    // reading the ping, so the text frame arrives FIRST (TCP order) and
    // the pong second. The server must accept that order.
    CHECK(sendServerFrame(fd, 0x9, "hb"));
    Frame getStateFirst;
    CHECK(readClientFrame(fd, getStateFirst));
    CHECK(getStateFirst.opcode == 0x1);
    int64_t getStateId = 0;
    {
        auto j = nlohmann::json::parse(getStateFirst.payload);
        CHECK(j["command"] == "get_state");
        getStateId = j["request_id"];
        CHECK(getStateId != 0);
    }
    Frame pong;
    CHECK(readClientFrame(fd, pong));
    CHECK(pong.opcode == 0xA);
    CHECK(pong.payload == "hb");

    // 2. A pushed event the client did not ask for (goes to onEvent).
    CHECK(sendServerFrame(fd, 0x1, R"({"command":"sync_status","request_id":0,"data":{}})"));

    // 3. get_state round-trip (response must echo the request id seen
    // in step 1).
    {
        nlohmann::json resp;
        resp["command"] = "response";
        resp["request_id"] = getStateId;
        resp["data"] = {{"is_logged_in", true},
                        {"user_id", "@u:hs"}};
        CHECK(sendServerFrame(fd, 0x1, resp.dump()));
    }

    // 4. jsoncmd ping with last_received_id.
    Frame ping;
    CHECK(readClientFrame(fd, ping));
    {
        auto j = nlohmann::json::parse(ping.payload);
        CHECK(j["command"] == "ping");
        CHECK(j["data"]["last_received_id"] == 42);
        nlohmann::json resp;
        resp["command"] = "pong";
        resp["request_id"] = j["request_id"];
        resp["data"] = nlohmann::json::object();
        CHECK(sendServerFrame(fd, 0x1, resp.dump()));
    }

    // 5. Fragmented text message: two frames, one logical message.
    CHECK(sendServerFrame(fd, 0x1, R"({"command":"sync_status")", false));
    CHECK(sendServerFrame(fd, 0x0, R"(,"request_id":0,"data":{}})"));

    // 6. Backend error reply surfaces as failure.
    Frame bogus;
    CHECK(readClientFrame(fd, bogus));
    {
        auto j = nlohmann::json::parse(bogus.payload);
        nlohmann::json resp;
        resp["command"] = "error";
        resp["request_id"] = j["request_id"];
        resp["data"] = "unknown command";
        CHECK(sendServerFrame(fd, 0x1, resp.dump()));
    }
    close(fd);
}

} // namespace

int main() {
    // --- URL parsing (no socket) ---
    {
        WsEndpoint ep;
        std::string err;
        CHECK(parseWsUrl("ws://127.0.0.1:8080/_gomuks/websocket", ep, err));
        CHECK(ep.host == "127.0.0.1" && ep.port == 8080 && !ep.tls);
        CHECK(ep.resource == "/_gomuks/websocket");
        CHECK(parseWsUrl("wss://example.org/x", ep, err));
        CHECK(ep.tls && ep.port == 443 && ep.resource == "/x");
        CHECK(parseWsUrl("ws://[::1]:9000/", ep, err));
        CHECK(ep.host == "::1" && ep.port == 9000);
        CHECK(!parseWsUrl("http://127.0.0.1:8080/x", ep, err));
        CHECK(!parseWsUrl("ws://:abc/", ep, err));
        CHECK(!parseWsUrl("noscheme", ep, err));
    }
    // --- Envelope helpers (no socket) ---
    {
        std::string wire =
            formatCommand("get_state", 7, nlohmann::json::object());
        JsonEnvelope env;
        std::string err;
        CHECK(parseEnvelope(wire, env, err));
        CHECK(env.command == "get_state" && env.requestId == 7);
        CHECK(!parseEnvelope("{oops", env, err));
        CHECK(!parseEnvelope(R"({"request_id":1})", env, err));
        CHECK(parseEnvelope(R"({"command":"pong","request_id":3})", env,
                            err));
        CHECK(env.command == "pong" && env.requestId == 3);
        CHECK(env.data.is_object());
    }

    // --- Live session against the fake backend ---
    std::promise<int> portPromise;
    auto portFuture = portPromise.get_future();
    std::thread server(fakeBackend, std::move(portPromise));
    int port = portFuture.get();

    GomuksClient client;
    client.setSession("http://127.0.0.1:" + std::to_string(port),
                      "test-cookie");
    int events = 0;
    std::string lastEvent;
    client.setEventHandler([&](const std::string& c, const nlohmann::json&) {
        events++;
        lastEvent = c;
    });
    std::string error;
    CHECK(client.connect(5, error));
    if (failures != 0)
        std::cerr << "connect failed: " << error << std::endl;

    nlohmann::json state;
    CHECK(client.getState(state, 5, error));
    CHECK(state.value("is_logged_in", false));
    CHECK(state.value("user_id", "") == "@u:hs");
    CHECK(events >= 1); // the pushed sync_status went to onEvent

    CHECK(client.ping(42, 5, error));

    // Fragmented sync_status arrives whole through nextEvent.
    {
        std::string command;
        nlohmann::json data;
        CHECK(client.nextEvent(command, data, 5));
        CHECK(command == "sync_status");
    }

    nlohmann::json dummy;
    CHECK(!client.call("bogus", nlohmann::json::object(), dummy, 5, error));
    CHECK(error.find("unknown command") != std::string::npos);

    client.disconnect();
    server.join();

    if (failures == 0)
        std::cout << "test_gomuks_ws: all checks passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
