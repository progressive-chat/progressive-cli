#include "demo_handler.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>
#include <algorithm>

namespace matrixcli { namespace api {

using json = nlohmann::json;

DemoHandler::DemoHandler() {}

bool DemoHandler::checkError(const Request& req, Response& resp) {
    auto it = req.params.find("error");
    if (it == req.params.end()) return false;

    resp.content_type = "application/json";
    json err;

    if (it->second == "rate_limit" || it->second == "ratelimit") {
        resp.status = 429;
        err = {{"errcode", "M_LIMIT_EXCEEDED"}, {"error", "Too many requests (demo simulation)"},
               {"retry_after_ms", 5000}};
    } else if (it->second == "forbidden") {
        resp.status = 403;
        err = {{"errcode", "M_FORBIDDEN"}, {"error", "You are not allowed to access this (demo simulation)"}};
    } else if (it->second == "unauthorized") {
        resp.status = 401;
        err = {{"errcode", "M_UNKNOWN_TOKEN"}, {"error", "Invalid access token (demo simulation)"}};
    } else if (it->second == "not_found") {
        resp.status = 404;
        err = {{"errcode", "M_NOT_FOUND"}, {"error", "Resource not found (demo simulation)"}};
    } else if (it->second == "server_error" || it->second == "500") {
        resp.status = 500;
        err = {{"errcode", "M_UNKNOWN"}, {"error", "Internal server error (demo simulation)"}};
    } else if (it->second == "too_large") {
        resp.status = 413;
        err = {{"errcode", "M_TOO_LARGE"}, {"error", "Request too large (demo simulation)"}};
    } else {
        return false;
    }

    resp.body = err.dump(2);
    return true;
}

Response DemoHandler::handleStatus(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    json j;
    j["logged_in"] = true;
    j["user_id"] = "@demo:demo.local";
    j["device_id"] = "DEMODEVICE";
    j["demo_mode"] = true;
    j["rooms_count"] = demo.rooms().size();
    j["messages_count"] = demo.allMessages().size();
    j["devices_count"] = demo.devices().size();
    j["formats"] = {"json", "text", "markdown", "gemini", "html"};
    j["endpoints"] = {
        "/api/status",
        "/api/rooms",
        "/api/rooms/:room_id/messages?limit=&from=&before=&format=",
        "/api/rooms/:room_id/members?format=&membership=",
        "/api/rooms/:room_id/state",
        "/api/devices",
        "/api/sync?since=&timeout=",
        "POST /api/send ({\"room_id\":\"...\", \"body\":\"...\"})"
    };
    resp.content_type = "application/json";
    resp.body = j.dump(2);
    return resp;
}

Response DemoHandler::handleRooms(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    auto format = req.format;

    if (format == Format::JSON) {
        resp.content_type = "application/json";
        json arr = json::array();
        for (auto& r : demo.rooms()) arr.push_back(demo::DemoData::renderRoomJson(r));
        resp.body = json{{"rooms", arr}, {"count", demo.rooms().size()}}.dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << "Demo Rooms (" << demo.rooms().size() << " total)\n"
            << "═════════════════════════════════\n\n";
        for (auto& r : demo.rooms()) oss << demo::DemoData::renderRoomText(r) << "\n";
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# Demo Rooms\n\n";
        for (auto& r : demo.rooms()) oss << demo::DemoData::renderRoomMarkdown(r);
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Demo Rooms</title>"
            << "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".room{border:1px solid #444;margin:8px 0;padding:12px;border-radius:4px;}"
            << ".name{color:#569cd6;font-size:1.2em;}.id{color:#6a9955;font-size:0.8em;}"
            << ".members{color:#ce9178;}.dm{color:#dcdcaa;background:#3c3c00;padding:1px 6px;border-radius:3px;}"
            << ".enc{color:#f44747;padding:1px 6px;border-radius:3px;}"
            << ".topic{color:#9cdcfe;margin-top:4px;}"
            << "</style></head><body><h1>Demo Rooms</h1>";
        for (auto& r : demo.rooms()) {
            oss << "<div class='room'><div class='name'>" << r.name;
            if (r.is_direct) oss << " <span class='dm'>DM</span>";
            if (r.is_encrypted) oss << " <span class='enc'>E2EE</span>";
            oss << "</div><div class='id'>" << r.id << "</div>"
                << "<div class='members'>" << r.members.size() << " members</div>";
            if (!r.topic.empty()) oss << "<div class='topic'>" << r.topic << "</div>";
            oss << "</div>";
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# Demo Rooms\n\n";
        for (auto& r : demo.rooms()) {
            oss << "=> /api/rooms/" << r.id << "/messages " << r.name;
            if (r.is_encrypted) oss << " [E2EE]";
            oss << "\n  " << r.members.size() << " members";
            if (r.is_direct) oss << " (DM)";
            if (!r.topic.empty()) oss << "\n  " << r.topic;
            oss << "\n\n";
        }
        resp.body = oss.str();
    }
    return resp;
}

Response DemoHandler::renderResponse(const demo::DemoRoom& room,
                                      const std::vector<demo::DemoMessage>& messages,
                                      Format format, int total) {
    Response resp;
    if (format == Format::JSON) {
        resp.content_type = "application/json";
        json j;
        j["room_id"] = room.id;
        j["room_name"] = room.name;
        j["count"] = messages.size();
        j["total"] = total;
        j["messages"] = json::array();
        for (auto& m : messages) {
            j["messages"].push_back(demo::DemoData::renderMessageJson(m));
        }
        if (!messages.empty()) {
            j["next_batch"] = messages.back().event_id;
            j["prev_batch"] = messages.front().event_id;
        }
        resp.body = j.dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << room.name << " (" << messages.size() << " of " << total << " messages)\n"
            << "══════════════════════════════════════\n\n";
        for (auto& m : messages) oss << demo::DemoData::renderMessageText(m);
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# " << room.name << "\n\n";
        for (auto& m : messages) oss << demo::DemoData::renderMessageMarkdown(m);
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>" << room.name
            << "</title><style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".msg{margin:6px 0;padding:8px;border-bottom:1px solid #333;}"
            << ".sender{color:#569cd6;font-weight:bold;}.time{color:#6a9955;font-size:0.7em;margin-left:10px;}"
            << ".body{color:#d4d4d4;margin-top:4px;}"
            << ".enc{color:#f44747;}.react{color:#dcdcaa;}"
            << "h1{color:#569cd6;}.count{color:#6a9955;}"
            << "</style></head><body><h1>" << room.name << "</h1>"
            << "<p class='count'>" << messages.size() << " of " << total << " messages</p>";
        for (auto& m : messages) {
            if (m.is_encrypted) {
                oss << "<div class='msg'><span class='sender'>" << m.sender_name
                    << "</span> <span class='enc'>[encrypted]</span></div>";
            } else if (!m.relates_to.empty()) {
                if (m.relation_type == "m.annotation") {
                    oss << "<div class='msg'><span class='sender'>" << m.sender_name
                        << "</span> <span class='react'>reacted with "
                        << (m.body.empty() ? "👍" : m.body) << "</span></div>";
                } else {
                    oss << "<div class='msg'><span class='sender'>" << m.sender_name
                        << " (edited)</span><div class='body'>" << m.body << "</div></div>";
                }
            } else {
                oss << "<div class='msg'><span class='sender'>" << m.sender_name
                    << "</span><span class='time'>" << m.timestamp
                    << "</span><div class='body'>";
                if (m.msgtype == "m.emote") oss << "* ";
                oss << m.body;
                if (m.msgtype == "m.image") oss << " <br><em>image " << m.width << "×" << m.height << "</em>";
                else if (m.msgtype == "m.file") oss << " <br><em>file: " << m.mimetype << " (" << m.size << "B)</em>";
                oss << "</div></div>";
            }
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# " << room.name << "\n\n";
        for (auto& m : messages) {
            if (m.is_encrypted) {
                oss << "> [encrypted]\n";
                oss << "-- " << m.sender_name << " 🔒\n\n";
            } else if (!m.relates_to.empty()) {
                if (m.relation_type == "m.annotation") {
                    oss << m.sender_name << " reacted with " << (m.body.empty() ? "👍" : m.body) << "\n\n";
                } else {
                    oss << "> " << m.body << "\n";
                    oss << "-- " << m.sender_name << " (edited)\n\n";
                }
            } else {
                oss << "> " << m.body << "\n";
                oss << "-- " << m.sender_name;
                if (m.msgtype == "m.image") oss << " [image]";
                else if (m.msgtype == "m.file") oss << " [file]";
                oss << "\n\n";
            }
        }
        resp.body = oss.str();
    }
    return resp;
}

Response DemoHandler::handleRoomMessages(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    auto room_id_it = req.params.find("room_id");
    if (room_id_it == req.params.end()) {
        resp.status = 400;
        resp.content_type = "application/json";
        resp.body = R"({"error":"missing room_id"})";
        return resp;
    }

    auto* room = demo.roomById(room_id_it->second);
    if (!room) {
        resp.status = 404;
        resp.content_type = "application/json";
        resp.body = R"({"error":"room not found"})";
        return resp;
    }

    int limit = 20;
    auto limit_it = req.params.find("limit");
    if (limit_it != req.params.end()) {
        limit = std::max(1, std::stoi(limit_it->second));
    }

    std::string from;
    auto from_it = req.params.find("from");
    if (from_it != req.params.end()) from = from_it->second;

    std::string before;
    auto before_it = req.params.find("before");
    if (before_it != req.params.end()) before = before_it->second;

    auto all = demo.messagesForRoom(room_id_it->second);
    int total = all.size();
    auto msgs = demo.messagesForRoom(room_id_it->second, from, before, limit);

    return renderResponse(*room, msgs, req.format, total);
}

Response DemoHandler::handleRoomMembers(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    auto room_id_it = req.params.find("room_id");
    if (room_id_it == req.params.end()) {
        resp.status = 400;
        resp.content_type = "application/json";
        resp.body = R"({"error":"missing room_id"})";
        return resp;
    }

    auto* room = demo.roomById(room_id_it->second);
    if (!room) {
        resp.status = 404;
        resp.content_type = "application/json";
        resp.body = R"({"error":"room not found"})";
        return resp;
    }

    // filter by membership if requested
    std::string membership_filter;
    auto mem_it = req.params.find("membership");
    if (mem_it != req.params.end()) membership_filter = mem_it->second;

    std::vector<demo::DemoMember> filtered;
    for (auto& m : room->members) {
        if (membership_filter.empty() || m.membership == membership_filter) {
            filtered.push_back(m);
        }
    }

    auto format = req.format;
    if (format == Format::JSON) {
        resp.content_type = "application/json";
        json j;
        j["room_id"] = room->id;
        j["members"] = json::array();
        for (auto& m : filtered) j["members"].push_back(demo::DemoData::renderMemberJson(m));
        j["total"] = filtered.size();
        resp.body = j.dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << room->name << " — Members (" << filtered.size() << ")\n"
            << "══════════════════════════════\n\n";
        for (auto& m : filtered) oss << demo::DemoData::renderMemberText(m);
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# " << room->name << " — Members\n\n";
        for (auto& m : filtered) {
            oss << "- **" << m.display_name << "** (`" << m.user_id << "`)";
            if (m.power_level >= 100) oss << " *admin*";
            else if (m.power_level >= 50) oss << " *mod*";
            oss << " — " << m.membership << "\n";
        }
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>" << room->name
            << " Members</title><style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".member{margin:4px 0;padding:4px 8px;}.name{color:#569cd6;}.userid{color:#6a9955;}"
            << ".admin{color:#ce9178;}.mod{color:#dcdcaa;}"
            << "</style></head><body><h1>" << room->name << " — Members</h1>";
        for (auto& m : filtered) {
            oss << "<div class='member'><span class='name'>" << m.display_name << "</span>"
                << " <span class='userid'>" << m.user_id << "</span>";
            if (m.power_level >= 100) oss << " <span class='admin'>admin</span>";
            else if (m.power_level >= 50) oss << " <span class='mod'>mod</span>";
            oss << " — " << m.membership << "</div>";
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# " << room->name << " — Members\n\n";
        for (auto& m : filtered) {
            oss << "### " << m.display_name << "\n"
                << m.user_id;
            if (m.power_level >= 100) oss << " (admin)";
            else if (m.power_level >= 50) oss << " (mod)";
            oss << " — " << m.membership << "\n\n";
        }
        resp.body = oss.str();
    }
    return resp;
}

Response DemoHandler::handleRoomState(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    auto room_id_it = req.params.find("room_id");
    if (room_id_it == req.params.end()) {
        resp.status = 400;
        resp.content_type = "application/json";
        resp.body = R"({"error":"missing room_id"})";
        return resp;
    }

    auto* room = demo.roomById(room_id_it->second);
    if (!room) {
        resp.status = 404;
        resp.content_type = "application/json";
        resp.body = R"({"error":"room not found"})";
        return resp;
    }

    resp.content_type = "application/json";
    json j;
    j["room_id"] = room->id;
    j["state"] = json::array();
    for (auto& e : room->state_events) {
        j["state"].push_back(demo::DemoData::renderStateEventJson(e));
    }
    resp.body = j.dump(2);
    return resp;
}

Response DemoHandler::handleSync(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();

    // Long-poll support: if timeout specified, wait briefly
    int timeout = 0;
    auto to_it = req.params.find("timeout");
    if (to_it != req.params.end()) {
        timeout = std::min(std::stoi(to_it->second), 10000); // max 10s
        if (timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout, 500)));
        }
    }

    resp.content_type = "application/json";
    json result;
    result["next_batch"] = "demo_sync_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    json rooms;
    for (auto& r : demo.rooms()) {
        auto msgs = demo.messagesForRoom(r.id, "", "", 10);
        json timeline, events = json::array();
        for (auto& m : msgs) {
            events.push_back(demo::DemoData::renderMessageJson(m));
        }
        timeline["events"] = events;
        timeline["limited"] = (int)msgs.size() >= 10;

        json state;
        for (auto& e : r.state_events) {
            state.push_back(demo::DemoData::renderStateEventJson(e));
        }

        json room_data;
        room_data["timeline"] = timeline;
        room_data["state"] = {"events", state};
        rooms[r.id] = room_data;
    }

    result["rooms"] = {{"join", rooms}};

    // Include device lists
    result["device_lists"] = {
        {"changed", json::array()},
        {"left", json::array()}
    };

    resp.body = result.dump(2);
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

        // Validate msgtype
        static const std::vector<std::string> valid_msgtypes = {
            "m.text", "m.notice", "m.emote", "m.image", "m.file", "m.video", "m.audio"
        };
        if (!msgtype.empty() &&
            std::find(valid_msgtypes.begin(), valid_msgtypes.end(), msgtype) == valid_msgtypes.end()) {
            resp.status = 400;
            resp.body = R"({"error":"invalid msgtype","valid_types":["m.text","m.notice","m.emote","m.image","m.file","m.video","m.audio"]})";
            return resp;
        }

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        demo::DemoMessage msg;
        msg.event_id = "$demo_" + std::to_string(now);
        msg.room_id = room_id;
        msg.sender = "@demo:demo.local";
        msg.sender_name = "You (demo)";
        msg.body = body;
        msg.msgtype = msgtype.empty() ? "m.text" : msgtype;
        msg.timestamp = now;

        // Handle media fields
        if (j.contains("url")) msg.url = j["url"].get<std::string>();
        if (j.contains("mimetype")) msg.mimetype = j["mimetype"].get<std::string>();
        if (j.contains("size")) msg.size = j["size"].get<int64_t>();
        if (j.contains("width")) msg.width = j["width"].get<int64_t>();
        if (j.contains("height")) msg.height = j["height"].get<int64_t>();

        demo.addMessage(msg);

        json result;
        result["event_id"] = msg.event_id;
        result["status"] = "sent";
        result["room_id"] = room_id;
        resp.body = result.dump(2);
        resp.status = 200;
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.body = json{{"error", e.what()}}.dump();
    }

    return resp;
}

Response DemoHandler::handleDevices(const Request& req) {
    Response resp;
    if (checkError(req, resp)) return resp;

    auto& demo = demo::DemoData::instance();
    auto format = req.format;

    if (format == Format::JSON) {
        resp.content_type = "application/json";
        json j;
        j["devices"] = json::array();
        for (auto& d : demo.devices()) j["devices"].push_back(demo::DemoData::renderDeviceJson(d));
        j["total"] = demo.devices().size();
        resp.body = j.dump(2);
    } else if (format == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << "Demo Devices (" << demo.devices().size() << ")\n"
            << "══════════════════════════════\n\n";
        for (auto& d : demo.devices()) oss << demo::DemoData::renderDeviceText(d);
        resp.body = oss.str();
    } else if (format == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# Demo Devices\n\n";
        for (auto& d : demo.devices()) {
            oss << "- **" << d.display_name << "** (`" << d.device_id << "`)"
                << (d.verified ? " ✅" : " ⚠️") << "\n";
        }
        resp.body = oss.str();
    } else if (format == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Devices</title>"
            << "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".device{margin:6px 0;padding:8px;border-bottom:1px solid #333;}"
            << ".name{color:#569cd6;}.id{color:#6a9955;font-size:0.8em;}"
            << ".verified{color:#4ec9b0;}.unverified{color:#ce9178;}"
            << "</style></head><body><h1>Devices</h1>";
        for (auto& d : demo.devices()) {
            oss << "<div class='device'><span class='name'>" << d.display_name << "</span>"
                << " <span class='id'>" << d.device_id << "</span>"
                << " <span class='" << (d.verified ? "verified" : "unverified") << "'>"
                << (d.verified ? "VERIFIED" : "UNVERIFIED") << "</span>"
                << "</div>";
        }
        oss << "</body></html>";
        resp.body = oss.str();
    } else if (format == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# Demo Devices\n\n";
        for (auto& d : demo.devices()) {
            oss << "### " << d.display_name << "\n"
                << d.device_id << "\n"
                << (d.verified ? "Verified ✅" : "Unverified ⚠️") << "\n\n";
        }
        resp.body = oss.str();
    }
    return resp;
}

void DemoHandler::registerRoutes(Router& router) {
    router.get("/api/status", [this](const Request& req) { return handleStatus(req); });
    router.get("/api/rooms", [this](const Request& req) { return handleRooms(req); });
    router.get("/api/rooms/:room_id/messages", [this](const Request& req) { return handleRoomMessages(req); });
    router.get("/api/rooms/:room_id/members", [this](const Request& req) { return handleRoomMembers(req); });
    router.get("/api/rooms/:room_id/state", [this](const Request& req) { return handleRoomState(req); });
    router.get("/api/devices", [this](const Request& req) { return handleDevices(req); });
    router.get("/api/sync", [this](const Request& req) { return handleSync(req); });
    router.post("/api/send", [this](const Request& req) { return handleSendMessage(req); });
}

void DemoHandler::save() {
    if (!_persistPath.empty()) {
        demo::DemoData::instance().save(_persistPath);
    }
}

void DemoHandler::load() {
    if (!_persistPath.empty()) {
        demo::DemoData::instance().load(_persistPath);
    }
}

}} // namespace matrixcli::api
