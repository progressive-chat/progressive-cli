#include "demo_handler.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>

namespace matrixcli { namespace api {

using json = nlohmann::json;

Response DemoHandler::handleStatus(const Request& req) {
    Response resp;
    resp.content_type = "application/json";
    resp.body = demo::DemoData::instance().statusToJson().dump(2);
    return resp;
}

Response DemoHandler::handleRooms(const Request& req) {
    auto& demo = demo::DemoData::instance();
    auto format = req.format;

    Response resp;
    if (format == Format::JSON) {
        resp.content_type = "application/json";
        resp.body = demo.roomsToJson("json").dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << "Demo Rooms (" << demo.rooms().size() << " total)\n";
        oss << "═════════════════════════════════\n\n";
        for (const auto& r : demo.rooms()) {
            oss << demo::DemoData::renderRoomText(r) << "\n";
        }
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# Demo Rooms\n\n";
        for (const auto& r : demo.rooms()) {
            oss << demo::DemoData::renderRoomMarkdown(r);
        }
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Demo Rooms</title>"
            << "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".room{border:1px solid #444;margin:8px 0;padding:12px;border-radius:4px;}"
            << ".name{color:#569cd6;font-size:1.2em;}"
            << ".id{color:#6a9955;font-size:0.8em;}"
            << ".members{color:#ce9178;}"
            << ".dm{color:#dcdcaa;background:#3c3c00;padding:1px 6px;border-radius:3px;}"
            << ".topic{color:#9cdcfe;margin-top:4px;}"
            << "</style></head><body><h1>Demo Rooms</h1>";
        for (const auto& r : demo.rooms()) {
            oss << "<div class='room'>"
                << "<div class='name'>" << r.name;
            if (r.is_direct) oss << " <span class='dm'>DM</span>";
            oss << "</div>"
                << "<div class='id'>" << r.id << "</div>"
                << "<div class='members'>" << r.member_count << " members</div>";
            if (!r.topic.empty()) oss << "<div class='topic'>" << r.topic << "</div>";
            oss << "</div>";
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# Demo Rooms\n\n";
        for (const auto& r : demo.rooms()) {
            oss << "=> /api/rooms/" << r.id << "/messages " << r.name << "\n";
            if (!r.topic.empty()) oss << "  " << r.topic << "\n";
            oss << "  " << r.member_count << " members" << (r.is_direct ? " (DM)" : "") << "\n\n";
        }
        resp.body = oss.str();
    }
    return resp;
}

Response DemoHandler::handleRoomMessages(const Request& req) {
    auto& demo = demo::DemoData::instance();
    auto format = req.format;

    std::string room_id = req.params.at("room_id");
    auto* room = demo.roomById(room_id);
    if (!room) {
        Response resp;
        resp.status = 404;
        resp.content_type = "application/json";
        resp.body = R"({"error":"room not found"})";
        return resp;
    }

    int limit = 20;
    if (req.params.contains("limit")) {
        limit = std::stoi(req.params.at("limit"));
    }

    auto msgs = demo.messagesForRoom(room_id);
    int count = (limit > 0 && limit < (int)msgs.size()) ? limit : (int)msgs.size();

    Response resp;
    if (format == Format::JSON) {
        resp.content_type = "application/json";
        json j;
        j["room_id"] = room_id;
        j["room_name"] = room->name;
        j["count"] = count;
        j["total"] = msgs.size();
        j["messages"] = json::array();
        for (int i = 0; i < count; i++) {
            j["messages"].push_back(demo::DemoData::renderMessageJson(msgs[i]));
        }
        resp.body = j.dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << room->name << " (" << count << " of " << msgs.size() << " messages)\n";
        oss << "══════════════════════════════════════\n\n";
        for (int i = 0; i < count; i++) {
            oss << demo::DemoData::renderMessageText(msgs[i]);
        }
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# " << room->name << "\n\n";
        for (int i = 0; i < count; i++) {
            oss << demo::DemoData::renderMessageMarkdown(msgs[i]);
        }
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>" << room->name
            << "</title><style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".msg{margin:6px 0;padding:8px;border-bottom:1px solid #333;}"
            << ".sender{color:#569cd6;font-weight:bold;}.time{color:#6a9955;font-size:0.7em;margin-left:10px;}"
            << ".body{color:#d4d4d4;margin-top:4px;}"
            << "h1{color:#569cd6;}.count{color:#6a9955;}"
            << "</style></head><body>"
            << "<h1>" << room->name << "</h1>"
            << "<p class='count'>" << count << " of " << msgs.size() << " messages</p>";
        for (int i = 0; i < count; i++) {
            oss << "<div class='msg'><span class='sender'>" << msgs[i].sender_name
                << "</span><span class='time'>" << "ts:" << msgs[i].timestamp
                << "</span><div class='body'>" << msgs[i].body << "</div></div>";
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# " << room->name << "\n\n";
        for (int i = 0; i < count; i++) {
            oss << "> " << msgs[i].body << "\n";
            oss << "-- " << msgs[i].sender_name << "\n\n";
        }
        resp.body = oss.str();
    }
    return resp;
}

Response DemoHandler::handleSync(const Request& req) {
    Response resp;
    resp.content_type = "application/json";
    resp.body = demo::DemoData::instance().syncToJson().dump(2);
    return resp;
}

Response DemoHandler::handleSendMessage(const Request& req) {
    Response resp;
    resp.content_type = "application/json";

    try {
        auto j = json::parse(req.body);
        std::string room_id = j.value("room_id", "");
        std::string body = j.value("body", "");
        std::string msgtype = j.value("msgtype", "m.text");

        if (room_id.empty() || body.empty()) {
            resp.status = 400;
            resp.body = R"({"error":"room_id and body are required"})";
            return resp;
        }

        auto& demo = demo::DemoData::instance();
        auto* room = demo.roomById(room_id);
        if (!room) {
            resp.status = 404;
            resp.body = R"({"error":"room not found"})";
            return resp;
        }

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string event_id = "$demo_" + std::to_string(now) + "_" +
                               std::to_string(demo.allMessages().size());

        demo::DemoMessage msg;
        msg.event_id = event_id;
        msg.room_id = room_id;
        msg.sender = "@demo:demo.local";
        msg.sender_name = "You (demo)";
        msg.body = body;
        msg.msgtype = msgtype;
        msg.timestamp = now;

        demo.addMessage(msg);

        json result;
        result["event_id"] = event_id;
        result["status"] = "sent";
        resp.body = result.dump(2);
        resp.status = 200;
    } catch (const std::exception& e) {
        resp.status = 400;
        json err;
        err["error"] = e.what();
        resp.body = err.dump();
    }

    return resp;
}

void DemoHandler::registerRoutes(Router& router) {
    router.get("/api/status", [this](const Request& req) {
        return handleStatus(req);
    });
    router.get("/api/rooms", [this](const Request& req) {
        return handleRooms(req);
    });
    router.get("/api/rooms/:room_id/messages", [this](const Request& req) {
        return handleRoomMessages(req);
    });
    router.get("/api/sync", [this](const Request& req) {
        return handleSync(req);
    });
    router.post("/api/send", [this](const Request& req) {
        return handleSendMessage(req);
    });
}

}} // namespace matrixcli::api
