#include "websocket.hpp"
#include "../util/logger.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <cstring>
#include <sstream>
#include <random>
#include <nlohmann/json.hpp>
#include <chrono>

namespace matrixcli { namespace api {

// ── Base64 encode (OpenSSL) ──
static std::string b64(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    BIO_write(b64, data, len);
    BIO_flush(b64);
    char* out;
    long olen = BIO_get_mem_data(mem, &out);
    std::string result(out, olen);
    BIO_free_all(b64);
    return result;
}

// ── WebSocket accept key ──
static std::string wsAcceptKey(const std::string& clientKey) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = clientKey + magic;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)combined.c_str(), combined.size(), hash);

    return b64(hash, SHA_DIGEST_LENGTH);
}

WebSocketSession::WebSocketSession(int fd) : _fd(fd) {}

WebSocketSession::~WebSocketSession() {
    if (_fd >= 0) close(_fd);
}

bool WebSocketSession::handleHandshake(const std::string& request) {
    // Extract Sec-WebSocket-Key
    auto pos = request.find("Sec-WebSocket-Key: ");
    if (pos == std::string::npos) return false;
    pos += 19;
    auto end = request.find("\r\n", pos);
    std::string key = request.substr(pos, end - pos);

    std::string accept = wsAcceptKey(key);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept << "\r\n"
         << "\r\n";

    std::string r = resp.str();
    return send(_fd, r.c_str(), r.size(), 0) == (ssize_t)r.size();
}

bool WebSocketSession::readFrame(std::string& message) {
    unsigned char hdr[2];
    ssize_t n = recv(_fd, hdr, 2, MSG_WAITALL);
    if (n != 2) { _open = false; return false; }

    bool fin = (hdr[0] & 0x80) != 0;
    int opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    if (len == 126) {
        unsigned char ext[2];
        if (recv(_fd, ext, 2, MSG_WAITALL) != 2) { _open = false; return false; }
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (recv(_fd, ext, 8, MSG_WAITALL) != 8) { _open = false; return false; }
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    if (len > 1024 * 1024) { _open = false; return false; } // 1MB limit

    unsigned char mask[4] = {0};
    if (masked) {
        if (recv(_fd, mask, 4, MSG_WAITALL) != 4) { _open = false; return false; }
    }

    std::vector<unsigned char> payload(len);
    size_t total = 0;
    while (total < len) {
        n = recv(_fd, payload.data() + total, len - total, 0);
        if (n <= 0) { _open = false; return false; }
        total += n;
    }

    if (masked) {
        for (size_t i = 0; i < len; i++) payload[i] ^= mask[i % 4];
    }

    // Handle control frames
    if (opcode == 0x8) { // Close
        _open = false;
        return false;
    }
    if (opcode == 0x9) { // Ping
        unsigned char pong[] = {0x8A, 0x00};
        send(_fd, pong, 2, 0);
        return false;
    }

    if (opcode == 0x1 || opcode == 0x2) { // Text or Binary
        message.assign(payload.begin(), payload.end());
        return fin;
    }

    return false;
}

bool WebSocketSession::sendFrame(const std::string& message) {
    if (!_open) return false;

    std::vector<unsigned char> frame;
    frame.push_back(0x81); // FIN + text opcode

    size_t len = message.size();
    if (len <= 125) {
        frame.push_back((unsigned char)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) frame.push_back((len >> (i * 8)) & 0xFF);
    }

    frame.insert(frame.end(), message.begin(), message.end());

    ssize_t sent = send(_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == (ssize_t)frame.size();
}

bool WebSocketSession::sendPing() {
    unsigned char ping[] = {0x89, 0x00};
    return send(_fd, ping, 2, MSG_NOSIGNAL) == 2;
}

bool WebSocketSession::sendClose() {
    unsigned char closeFrame[] = {0x88, 0x00};
    send(_fd, closeFrame, 2, MSG_NOSIGNAL);
    _open = false;
    return true;
}

// ── EventBus ──

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

void EventBus::subscribe(WebSocketSession* session) {
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.push_back(session);
    util::Logger::instance().info("WebSocket client connected (total: " +
        std::to_string(_sessions.size()) + ")");
}

void EventBus::unsubscribe(WebSocketSession* session) {
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.erase(std::remove(_sessions.begin(), _sessions.end(), session), _sessions.end());
}

void EventBus::publish(const std::string& channel, const std::string& message) {
    broadcast(channel, message);
}

void EventBus::broadcast(const std::string& channel, const std::string& message) {
    std::lock_guard<std::mutex> lock(_mutex);
    nlohmann::json j;
    j["channel"] = channel;
    j["message"] = message;
    j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string payload = j.dump();
    for (auto* s : _sessions) {
        if (s->isOpen()) s->sendFrame(payload);
    }
}

}} // namespace matrixcli::api
