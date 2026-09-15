#pragma once
// Minimal RFC 6455 WebSocket client: text messages, ping/pong, close —
// plain ws:// and TLS wss:// over POSIX sockets + OpenSSL.
//
// Written for the gomuks backend API (GET /_gomuks/websocket), but the
// transport itself is generic: connect() performs the HTTP upgrade,
// sendText()/recvText() move text messages, ping frames are answered
// automatically inside recvText().
#include <cstdint>
#include <map>
#include <string>

namespace matrixcli { namespace gomuks {

struct WsEndpoint {
    std::string host;
    int port = 80;
    std::string resource = "/";
    bool tls = false;
};

// Parse ws://host[:port]/path?query or wss://... (http/https schemes are
// rejected — this client only speaks the websocket upgrade).
bool parseWsUrl(const std::string& url, WsEndpoint& out, std::string& error);

// base64 (RFC 4648, with padding) for the handshake key and HTTP auth.
std::string base64Encode(const std::string& in);

class WsClient {
public:
    WsClient();
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Blocking connect + HTTP upgrade within timeoutSec. extraHeaders ride
    // along with the handshake (e.g. {"Cookie", "gomuks_auth=..."}).
    // insecure skips the TLS certificate/hostname check (loopback testing).
    bool connect(const std::string& url,
                 const std::map<std::string, std::string>& extraHeaders,
                 int timeoutSec, std::string& error, bool insecure = false);
    bool isOpen() const { return _open; }

    // Send one text message (client frames are always masked, as required).
    bool sendText(const std::string& payload, std::string& error);

    // Receive one text message. Ping frames are answered with pong
    // automatically, pong frames are skipped. Returns false on timeout,
    // close or error — see closedByPeer()/lastError().
    bool recvText(std::string& out, int timeoutSec);

    // Best-effort close handshake (code 1000), then hard close.
    void close(const std::string& reason = "");

    bool closedByPeer() const { return _peerClosed; }
    std::string lastError() const { return _lastError; }

private:
    bool writeN(const char* data, size_t len, int timeoutMs);
    bool readN(char* data, size_t len, int timeoutMs);
    bool readHttpResponse(std::string& statusLine,
                          std::map<std::string, std::string>& headers,
                          int timeoutMs);
    bool sendFrame(uint8_t opcode, const std::string& payload,
                   std::string& error);
    // Read one raw frame; handles ping/pong/close internally and returns
    // true only for a complete data message (fragmentation reassembled).
    bool recvFrame(uint8_t& opcode, std::string& payload, int timeoutMs);

    int _fd = -1;
    void* _ssl = nullptr;    // SSL* (OpenSSL headers stay in the .cpp)
    void* _sslCtx = nullptr; // SSL_CTX*
    bool _open = false;
    bool _peerClosed = false;
    bool _closeSent = false;
    std::string _lastError;
    std::string _rxBuf;
};

}} // namespace matrixcli::gomuks
