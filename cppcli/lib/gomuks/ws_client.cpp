// ws_client.cpp — RFC 6455 client: TCP/TLS dial, HTTP upgrade, masked
// client frames, server-frame decoding with ping auto-reply and close
// handling. Fragmented data messages are reassembled; control frames
// interleaved inside a fragmented message are honored.
#include "ws_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

namespace matrixcli { namespace gomuks {

namespace {

const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const size_t kMaxFrame = 4 * 1024 * 1024; // receives larger than this fail
const size_t kMaxHandshake = 16 * 1024;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

std::string base64Encode(const std::string& in) {
    if (in.empty()) return "";
    std::string out(((in.size() + 2) / 3) * 4 + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
    out.resize(n);
    return out;
}

bool parseWsUrl(const std::string& url, WsEndpoint& out, std::string& error) {
    out = WsEndpoint{};
    std::string scheme, rest;
    auto sep = url.find("://");
    if (sep == std::string::npos) {
        error = "websocket URL needs a ws:// or wss:// scheme";
        return false;
    }
    scheme = url.substr(0, sep);
    rest = url.substr(sep + 3);
    if (scheme == "ws") {
        out.tls = false;
        out.port = 80;
    } else if (scheme == "wss") {
        out.tls = true;
        out.port = 443;
    } else {
        error = "not a websocket URL (want ws:// or wss://): " + scheme;
        return false;
    }
    auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.resource = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (out.resource.empty()) out.resource = "/";
    // Strip userinfo if present (not used by the handshake).
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {
        // IPv6 literal [::1] or [::1]:port.
        auto close = authority.find(']');
        if (close == std::string::npos) {
            error = "bad IPv6 literal in websocket URL";
            return false;
        }
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            try {
                out.port = std::stoi(authority.substr(close + 2));
            } catch (...) {
                error = "bad port in websocket URL";
                return false;
            }
        }
    } else {
        auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            try {
                out.port = std::stoi(authority.substr(colon + 1));
            } catch (...) {
                error = "bad port in websocket URL";
                return false;
            }
        } else {
            out.host = authority;
        }
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535) {
        error = "bad host/port in websocket URL";
        return false;
    }
    return true;
}

WsClient::WsClient() = default;

WsClient::~WsClient() {
    close();
}

bool WsClient::writeN(const char* data, size_t len, int timeoutMs) {
    size_t done = 0;
    const int64_t deadline = nowMs() + timeoutMs;
    while (done < len) {
        if (_ssl) {
            SSL* ssl = static_cast<SSL*>(_ssl);
            int r = SSL_write(ssl, data + done, static_cast<int>(len - done));
            if (r > 0) {
                done += static_cast<size_t>(r);
                continue;
            }
            int err = SSL_get_error(ssl, r);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                _lastError = "TLS write failed";
                return false;
            }
            short ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            struct pollfd pfd{_fd, ev, 0};
            int ms = static_cast<int>(deadline - nowMs());
            if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
                _lastError = "TLS write timed out";
                return false;
            }
            continue;
        }
        struct pollfd pfd{_fd, POLLOUT, 0};
        int ms = static_cast<int>(deadline - nowMs());
        if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
            _lastError = "socket write timed out";
            return false;
        }
        ssize_t r = ::send(_fd, data + done, len - done, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            _lastError = std::string("socket write: ") + strerror(errno);
            return false;
        }
        done += static_cast<size_t>(r);
    }
    return true;
}

bool WsClient::readN(char* data, size_t len, int timeoutMs) {
    size_t done = 0;
    const int64_t deadline = nowMs() + timeoutMs;
    while (done < len) {
        if (_ssl) {
            SSL* ssl = static_cast<SSL*>(_ssl);
            int r = SSL_read(ssl, data + done, static_cast<int>(len - done));
            if (r > 0) {
                done += static_cast<size_t>(r);
                continue;
            }
            int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_ZERO_RETURN) {
                _lastError = "TLS connection closed";
                return false;
            }
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                _lastError = "TLS read failed";
                return false;
            }
            short ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            struct pollfd pfd{_fd, ev, 0};
            int ms = static_cast<int>(deadline - nowMs());
            if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
                _lastError = "TLS read timed out";
                return false;
            }
            continue;
        }
        struct pollfd pfd{_fd, POLLIN, 0};
        int ms = static_cast<int>(deadline - nowMs());
        if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
            _lastError = "socket read timed out";
            return false;
        }
        ssize_t r = ::recv(_fd, data + done, len - done, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            _lastError = std::string("socket read: ") + strerror(errno);
            return false;
        }
        if (r == 0) {
            _lastError = "connection closed by peer";
            return false;
        }
        done += static_cast<size_t>(r);
    }
    return true;
}

bool WsClient::readHttpResponse(std::string& statusLine,
                                std::map<std::string, std::string>& headers,
                                int timeoutMs) {
    // The handshake reply is small; accumulate until the header terminator.
    const int64_t deadline = nowMs() + timeoutMs;
    while (_rxBuf.find("\r\n\r\n") == std::string::npos) {
        if (_rxBuf.size() > kMaxHandshake) {
            _lastError = "handshake reply too large";
            return false;
        }
        struct pollfd pfd{_fd, POLLIN, 0};
        int ms = static_cast<int>(deadline - nowMs());
        if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
            _lastError = "handshake reply timed out";
            return false;
        }
        char tmp[4096];
        ssize_t r;
        if (_ssl) {
            SSL* ssl = static_cast<SSL*>(_ssl);
            r = SSL_read(ssl, tmp, sizeof(tmp));
            if (r <= 0) {
                int err = SSL_get_error(ssl, static_cast<int>(r));
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    continue;
                _lastError = "TLS read during handshake failed";
                return false;
            }
        } else {
            r = ::recv(_fd, tmp, sizeof(tmp), 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                _lastError = std::string("socket read during handshake: ") +
                             strerror(errno);
                return false;
            }
            if (r == 0) {
                _lastError = "connection closed during handshake";
                return false;
            }
        }
        _rxBuf.append(tmp, static_cast<size_t>(r));
    }
    size_t end = _rxBuf.find("\r\n\r\n");
    std::string block = _rxBuf.substr(0, end);
    _rxBuf.erase(0, end + 4);
    auto nl = block.find("\r\n");
    statusLine = (nl == std::string::npos) ? block : block.substr(0, nl);
    size_t pos = (nl == std::string::npos) ? std::string::npos : nl + 2;
    while (pos != std::string::npos && pos < block.size()) {
        auto eol = block.find("\r\n", pos);
        std::string line = block.substr(pos, eol == std::string::npos
                                                 ? std::string::npos
                                                 : eol - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos)
            headers[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    return true;
}

static bool dialTcp(const std::string& host, int port, int timeoutMs,
                    int& fdOut, std::string& error) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &list) != 0) {
        error = "cannot resolve " + host;
        return false;
    }
    const int64_t deadline = nowMs() + timeoutMs;
    bool ok = false;
    std::string lastErr = "no address";
    for (struct addrinfo* ai = list; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastErr = strerror(errno);
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno != EINPROGRESS) {
            lastErr = strerror(errno);
            ::close(fd);
            continue;
        }
        if (r != 0) {
            struct pollfd pfd{fd, POLLOUT, 0};
            int ms = static_cast<int>(deadline - nowMs());
            if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
                lastErr = "connect timed out";
                ::close(fd);
                continue;
            }
            int soErr = 0;
            socklen_t soLen = sizeof(soErr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soLen) != 0 ||
                soErr != 0) {
                lastErr = soErr ? strerror(soErr) : "connect failed";
                ::close(fd);
                continue;
            }
        }
        fdOut = fd;
        ok = true;
        break;
    }
    freeaddrinfo(list);
    if (!ok) error = "connect to " + host + ": " + lastErr;
    return ok;
}

bool WsClient::connect(const std::string& url,
                       const std::map<std::string, std::string>& extraHeaders,
                       int timeoutSec, std::string& error, bool insecure) {
    close();
    _lastError.clear();
    _peerClosed = false;
    WsEndpoint ep;
    if (!parseWsUrl(url, ep, error)) {
        _lastError = error;
        return false;
    }
    const int timeoutMs = timeoutSec * 1000;
    int fd = -1;
    if (!dialTcp(ep.host, ep.port, timeoutMs, fd, error)) {
        _lastError = error;
        return false;
    }
    _fd = fd;

    if (ep.tls) {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            error = "SSL_CTX_new failed";
            _lastError = error;
            ::close(_fd);
            _fd = -1;
            return false;
        }
        _sslCtx = ctx;
        if (!insecure) {
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            error = "SSL_new failed";
            _lastError = error;
            ::close(_fd);
            _fd = -1;
            return false;
        }
        _ssl = ssl;
        SSL_set_tlsext_host_name(ssl, ep.host.c_str());
        if (!insecure) SSL_set1_host(ssl, ep.host.c_str());
        SSL_set_fd(ssl, _fd);
        const int64_t deadline = nowMs() + timeoutMs;
        for (;;) {
            int r = SSL_connect(ssl);
            if (r == 1) break;
            int err = SSL_get_error(ssl, r);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                char buf[256];
                ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
                error = std::string("TLS handshake failed: ") + buf;
                _lastError = error;
                close();
                return false;
            }
            short ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            struct pollfd pfd{_fd, ev, 0};
            int ms = static_cast<int>(deadline - nowMs());
            if (ms <= 0 || poll(&pfd, 1, ms) <= 0) {
                error = "TLS handshake timed out";
                _lastError = error;
                close();
                return false;
            }
        }
        if (!insecure && SSL_get_verify_result(ssl) != X509_V_OK) {
            error = "TLS certificate verification failed";
            _lastError = error;
            close();
            return false;
        }
    }

    // The upgrade request. The key is 16 random bytes, base64-encoded.
    std::string rawKey(16, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(&rawKey[0]), 16);
    std::string key = base64Encode(rawKey);
    std::string req = "GET " + ep.resource +
                      " HTTP/1.1\r\nHost: " + ep.host + ":" +
                      std::to_string(ep.port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " +
                      key +
                      "\r\nSec-WebSocket-Version: 13\r\n";
    for (const auto& [name, value] : extraHeaders)
        req += name + ": " + value + "\r\n";
    req += "\r\n";
    if (!writeN(req.data(), req.size(), timeoutMs)) {
        error = _lastError;
        close();
        return false;
    }
    std::string status;
    std::map<std::string, std::string> headers;
    if (!readHttpResponse(status, headers, timeoutMs)) {
        error = _lastError;
        close();
        return false;
    }
    if (status.find(" 101") == std::string::npos) {
        error = "websocket upgrade rejected: " + status;
        _lastError = error;
        close();
        return false;
    }
    auto it = headers.find("Sec-WebSocket-Accept");
    if (it == headers.end()) {
        error = "websocket upgrade reply misses Sec-WebSocket-Accept";
        _lastError = error;
        close();
        return false;
    }
    unsigned char digest[SHA_DIGEST_LENGTH];
    std::string material = key + kWsGuid;
    SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(),
         digest);
    std::string expected = base64Encode(
        std::string(reinterpret_cast<char*>(digest), sizeof(digest)));
    if (it->second != expected) {
        error = "websocket accept key mismatch (not talking to a WS server?)";
        _lastError = error;
        close();
        return false;
    }
    _open = true;
    return true;
}

bool WsClient::sendFrame(uint8_t opcode, const std::string& payload,
                         std::string& error) {
    if (!_open) {
        error = "websocket not connected";
        return false;
    }
    if (payload.size() > kMaxFrame) {
        error = "websocket frame too large";
        return false;
    }
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    // Client-to-server frames are always masked (RFC 6455 section 5.1).
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    std::string mask(4, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(&mask[0]), 4);
    frame += mask;
    frame += payload;
    for (size_t i = 0; i < payload.size(); ++i)
        frame[frame.size() - payload.size() + i] ^=
            mask[i % 4];
    if (!writeN(frame.data(), frame.size(), 30000)) {
        error = _lastError;
        return false;
    }
    return true;
}

bool WsClient::sendText(const std::string& payload, std::string& error) {
    return sendFrame(0x1, payload, error);
}

bool WsClient::recvFrame(uint8_t& opcode, std::string& payload, int timeoutMs) {
    // Bytes left over from the HTTP upgrade (_rxBuf) are consumed before
    // touching the socket; needMore() below handles both uniformly.
    std::string message;
    uint8_t firstOpcode = 0;
    bool fragmented = false;
    for (;;) {
        // Fill the buffer until a full frame header is available.
        while (_rxBuf.size() < 2) {
            char tmp[4096];
            size_t want = 2 - _rxBuf.size();
            if (want > sizeof(tmp)) want = sizeof(tmp);
            if (!readN(tmp, want, timeoutMs)) return false;
            _rxBuf.append(tmp, want);
        }
        uint8_t b0 = static_cast<uint8_t>(_rxBuf[0]);
        uint8_t b1 = static_cast<uint8_t>(_rxBuf[1]);
        bool fin = (b0 & 0x80) != 0;
        uint8_t op = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t head = 2;
        if (masked) {
            // Servers must not mask; fail closed on violation.
            _lastError = "protocol error: masked server frame";
            return false;
        }
        auto needMore = [&](size_t n) -> bool {
            while (_rxBuf.size() < n) {
                char tmp[65536];
                size_t want = n - _rxBuf.size();
                if (want > sizeof(tmp)) want = sizeof(tmp);
                if (!readN(tmp, want, timeoutMs)) return false;
                _rxBuf.append(tmp, want);
            }
            return true;
        };
        if (len == 126) {
            if (!needMore(head + 2)) return false;
            len = (static_cast<uint64_t>(static_cast<uint8_t>(_rxBuf[2])) << 8) |
                  static_cast<uint64_t>(static_cast<uint8_t>(_rxBuf[3]));
            head += 2;
        } else if (len == 127) {
            if (!needMore(head + 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | static_cast<uint8_t>(_rxBuf[head + i]);
            head += 8;
            if (len >> 63) { // top bit set: absurd length
                _lastError = "protocol error: absurd frame length";
                return false;
            }
        }
        if (len > kMaxFrame) {
            _lastError = "protocol error: frame too large";
            return false;
        }
        if (!needMore(head + static_cast<size_t>(len))) return false;
        std::string data = _rxBuf.substr(head, static_cast<size_t>(len));
        _rxBuf.erase(0, head + static_cast<size_t>(len));

        if (op == 0x8) { // close
            _peerClosed = true;
            if (!_closeSent) {
                std::string err;
                sendFrame(0x8, "", err); // best-effort echo
                _closeSent = true;
            }
            _lastError = "connection closed by peer";
            return false;
        }
        if (op == 0x9) { // ping -> pong with identical payload
            if (!fin || data.size() > 125) {
                _lastError = "protocol error: bad ping";
                return false;
            }
            std::string err;
            if (!sendFrame(0xA, data, err)) {
                _lastError = err;
                return false;
            }
            continue;
        }
        if (op == 0xA) continue; // pong: nothing to do
        if (op == 0x0) {         // continuation
            if (!fragmented) {
                _lastError = "protocol error: stray continuation";
                return false;
            }
            message += data;
            if (fin) {
                opcode = firstOpcode;
                payload = message;
                return true;
            }
            continue;
        }
        if (op != 0x1 && op != 0x2) {
            _lastError = "protocol error: unknown opcode";
            return false;
        }
        if (fragmented) {
            _lastError = "protocol error: new message inside fragment";
            return false;
        }
        if (fin) {
            opcode = op;
            payload = data;
            return true;
        }
        fragmented = true;
        firstOpcode = op;
        message = data;
    }
}

bool WsClient::recvText(std::string& out, int timeoutSec) {
    if (!_open) {
        _lastError = "websocket not connected";
        return false;
    }
    uint8_t opcode = 0;
    std::string payload;
    if (!recvFrame(opcode, payload, timeoutSec * 1000)) return false;
    out = payload; // text and binary both surface as bytes; JSON is text
    return true;
}

void WsClient::close(const std::string& reason) {
    if (_open && !_closeSent && !_peerClosed) {
        std::string err;
        std::string payload("\x03\xE8", 2); // 1000 normal closure
        payload += reason;
        sendFrame(0x8, payload, err);
        _closeSent = true;
    }
    _open = false;
    if (_ssl) {
        SSL_shutdown(static_cast<SSL*>(_ssl));
        SSL_free(static_cast<SSL*>(_ssl));
        _ssl = nullptr;
    }
    if (_sslCtx) {
        SSL_CTX_free(static_cast<SSL_CTX*>(_sslCtx));
        _sslCtx = nullptr;
    }
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
    _rxBuf.clear();
}

}} // namespace matrixcli::gomuks
