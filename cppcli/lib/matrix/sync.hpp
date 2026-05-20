#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include "events.hpp"
#include "../http/http.hpp"
#include "client.hpp"

namespace matrixcli { namespace matrix {

class SyncLoop {
public:
    SyncLoop(http::Client& http, const Credentials& creds);
    ~SyncLoop();

    void start(std::function<void(const Event&)> onEvent);
    void stop();

    bool isRunning() const { return _running.load(); }

private:
    void run(std::function<void(const Event&)> onEvent);

    http::Client& _http;
    Credentials _creds;
    std::string _since;
    std::unique_ptr<std::thread> _thread;
    std::atomic<bool> _running{false};
};

}} // namespace matrixcli::matrix
