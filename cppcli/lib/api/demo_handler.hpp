#pragma once

#include "server.hpp"
#include "router.hpp"
#include "demo_data.hpp"
#include <memory>

namespace matrixcli { namespace api {

class DemoHandler {
public:
    DemoHandler();

    Response handleStatus(const Request& req);
    Response handleRooms(const Request& req);
    Response handleRoomMessages(const Request& req);
    Response handleRoomMembers(const Request& req);
    Response handleRoomState(const Request& req);
    Response handleSync(const Request& req);
    Response handleSendMessage(const Request& req);
    Response handleDevices(const Request& req);

    void registerRoutes(Router& router);

    void setPersistPath(const std::string& path) { _persistPath = path; }
    void save();
    void load();

private:
    Response renderResponse(const demo::DemoRoom& room,
                            const std::vector<demo::DemoMessage>& messages,
                            Format format, int total);
    bool checkError(const Request& req, Response& resp);

    std::string _persistPath;
};

}} // namespace matrixcli::api
