#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <vector>
#include <map>

namespace matrixcli { namespace api {

// Minimal WebSocket frame implementation
// Supports: text frames, ping/pong, close
// No compression, no fragmentation

class WebSocketSession {
public:
    using MessageHandler = std::function<void(const std::string&)>;

    explicit WebSocketSession(int fd);
    ~WebSocketSession();

    bool handleHandshake(const std::string& request);
    bool readFrame(std::string& message);
    bool sendFrame(const std::string& message);
    bool sendPing();
    bool sendClose();

    int fd() const { return _fd; }
    bool isOpen() const { return _open; }

private:
    int _fd;
    bool _open = true;
};

// Simple pub-sub event broker
class EventBus {
public:
    static EventBus& instance();

    void publish(const std::string& channel, const std::string& message);
    void subscribe(WebSocketSession* session);
    void unsubscribe(WebSocketSession* session);
    void broadcast(const std::string& channel, const std::string& message);

private:
    EventBus() = default;
    std::mutex _mutex;
    std::vector<WebSocketSession*> _sessions;
};

}} // namespace matrixcli::api
