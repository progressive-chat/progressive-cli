#pragma once

#include "server.hpp"
#include "router.hpp"
#include "demo_data.hpp"

namespace matrixcli { namespace api {

class DemoHandler {
public:
    Response handleStatus(const Request& req);
    Response handleRooms(const Request& req);
    Response handleRoomMessages(const Request& req);
    Response handleSync(const Request& req);
    Response handleSendMessage(const Request& req);

    void registerRoutes(Router& router);
};

}} // namespace matrixcli::api
