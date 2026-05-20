#include "sync.hpp"
#include "api.hpp"
#include "error.hpp"
#include "../util/logger.hpp"

namespace matrixcli { namespace matrix {

SyncLoop::SyncLoop(http::Client& http, const Credentials& creds)
    : _http(http), _creds(creds) {}

SyncLoop::~SyncLoop() { stop(); }

void SyncLoop::start(std::function<void(const Event&)> onEvent) {
    if (_running.exchange(true)) {
        return;
    }
    _thread = std::make_unique<std::thread>(&SyncLoop::run, this, std::move(onEvent));
}

void SyncLoop::stop() {
    if (!_running.exchange(false)) {
        return;
    }
    if (_thread && _thread->joinable()) {
        _thread->join();
    }
    _thread.reset();
}

void SyncLoop::run(std::function<void(const Event&)> onEvent) {
    util::Logger::instance().info("Sync loop started");

    while (_running.load()) {
        try {
            json j = api::sync(_http, _creds, _since);

            _since = j.value("next_batch", _since);

            if (j.contains("rooms") && j["rooms"].contains("join")) {
                for (auto& [room_id, room_data] : j["rooms"]["join"].items()) {
                    if (room_data.contains("timeline") &&
                        room_data["timeline"].contains("events")) {
                        for (auto& ev : room_data["timeline"]["events"]) {
                            Event event = Event::fromJSON(ev);
                            event.room_id = room_id;
                            if (onEvent) {
                                onEvent(event);
                            }
                        }
                    }
                }
            }

        } catch (const MatrixError& e) {
            util::Logger::instance().warn(std::string("Sync error: ") + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(5));
        } catch (const std::exception& e) {
            util::Logger::instance().error(std::string("Sync crash: ") + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }

    util::Logger::instance().info("Sync loop stopped");
}

}} // namespace matrixcli::matrix
