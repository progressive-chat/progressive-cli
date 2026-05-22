#include "demo_data.hpp"

#include <chrono>
#include <sstream>

namespace matrixcli { namespace demo {

DemoData& DemoData::instance() {
    static DemoData inst;
    return inst;
}

DemoData::DemoData() {
    generate();
}

void DemoData::generate() {
    _rooms = {
        {"!general:demo.local", "#general", "General discussion — welcome to matrixcli demo!", 42, false},
        {"!random:demo.local", "#random", "Random stuff and memes", 28, false},
        {"!dev:demo.local", "#dev", "Development chat — talk about C++, Matrix protocol, ncurses", 15, false},
        {"!announcements:demo.local", "#announcements", "Official announcements from the matrixcli team", 100, false},
        {"!dm_alice:demo.local", "Alice", "", 2, true},
        {"!dm_bob:demo.local", "Bob", "", 2, true},
    };

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t t = now;

    auto mk = [&](const std::string& event_id, const std::string& room,
                   const std::string& sender, const std::string& name,
                   const std::string& body, const std::string& msgtype = "m.text") {
        _messages.push_back({event_id, room, sender, name, body, msgtype, "", t});
        t -= 60 + (std::hash<std::string>{}(event_id) % 300);
    };

    // #general
    mk("$evt001", "!general:demo.local", "@alice:demo.local", "Alice",
       "Welcome everyone! This is a demo of matrixcli — a terminal Matrix client.");
    mk("$evt002", "!general:demo.local", "@bob:demo.local", "Bob",
       "Hey Alice! This looks great. The C++ rewrite is impressive.");
    mk("$evt003", "!general:demo.local", "@alice:demo.local", "Alice",
       "Thanks! It supports multiple output formats: JSON, text, markdown, gemini, and HTML.");
    mk("$evt004", "!general:demo.local", "@charlie:demo.local", "Charlie",
       "Does it support end-to-end encryption?");
    mk("$evt005", "!general:demo.local", "@alice:demo.local", "Alice",
       "Yes! It has full libolm bindings — Olm for 1-to-1 and Megolm for group encryption.");
    mk("$evt006", "!general:demo.local", "@bob:demo.local", "Bob",
       "Also: SOCKS5 and HTTP proxy support, custom DNS, and TLS certificate pinning.");
    mk("$evt007", "!general:demo.local", "@dave:demo.local", "Dave",
       "Can I use it with Tor?");
    mk("$evt008", "!general:demo.local", "@alice:demo.local", "Alice",
       "Absolutely! You can route through Tor, I2P, or Yggdrasil. Just select the connection type at login.");
    mk("$evt009", "!general:demo.local", "@bob:demo.local", "Bob",
       "The TUI is built with ncurses — full terminal UI with room list, message view, and composer.");
    mk("$evt010", "!general:demo.local", "@charlie:demo.local", "Charlie",
       "Check out the /help command in the TUI for all available slash commands!");

    // #random
    mk("$evt011", "!random:demo.local", "@bob:demo.local", "Bob",
       "Why did the C++ developer break up with the Java developer? Too many exceptions! 😄");
    mk("$evt012", "!random:demo.local", "@alice:demo.local", "Alice",
       "A SQL query walks into a bar, joins two tables and asks... can I get a date?");
    mk("$evt013", "!random:demo.local", "@charlie:demo.local", "Charlie",
       "Programmer: I have a problem. I'll use threads. Problem solved! ... Now I have tow prblmes.");

    // #dev
    mk("$evt014", "!dev:demo.local", "@alice:demo.local", "Alice",
       "The HTTP client is implemented with raw BSD sockets + OpenSSL — no external dependencies.");
    mk("$evt015", "!dev:demo.local", "@bob:demo.local", "Bob",
       "Nice! I'm working on adding WebRTC support for voice/video calls.");
    mk("$evt016", "!dev:demo.local", "@charlie:demo.local", "Charlie",
       "For the TUI, we use ncurses. Has anyone looked into Notcurses for better visuals?");
    mk("$evt017", "!dev:demo.local", "@alice:demo.local", "Alice",
       "Good idea! Notcurses supports images, video, and multi-threading. Could be a future upgrade.");
    mk("$evt018", "!dev:demo.local", "@bob:demo.local", "Bob",
       "The cmdspec system auto-generates TypeScript type declarations from Go code — we could do similar for C++.");

    // #announcements
    mk("$evt019", "!announcements:demo.local", "@admin:demo.local", "matrixcli-bot",
       "[ANN] matrixcli v0.1.0 released — First C++ release with full Matrix CS API, E2EE, and TUI.");
    mk("$evt020", "!announcements:demo.local", "@admin:demo.local", "matrixcli-bot",
       "[ANN] New feature: multi-format REST API — get your data as JSON, plain text, Markdown, Gemtext, or HTML.");
    mk("$evt021", "!announcements:demo.local", "@admin:demo.local", "matrixcli-bot",
       "[ANN] Security: TLS certificate pinning is now available for all connections.");

    // DM Alice
    mk("$evt022", "!dm_alice:demo.local", "@alice:demo.local", "Alice",
       "Hey! This is a private DM. E2EE ensures only we can read these messages.");
    mk("$evt023", "!dm_alice:demo.local", "@demo:demo.local", "You",
       "Cool! So the Olm session is established between our devices?");
    mk("$evt024", "!dm_alice:demo.local", "@alice:demo.local", "Alice",
       "Exactly. Each device gets its own Olm session. Megolm is used for group chats.");
    mk("$evt025", "!dm_alice:demo.local", "@demo:demo.local", "You",
       "And the keys are stored in SQLite for offline access?");
    mk("$evt026", "!dm_alice:demo.local", "@alice:demo.local", "Alice",
       "Yes! The database caches events, sync tokens, and crypto keys — everything works offline.");

    // DM Bob
    mk("$evt027", "!dm_bob:demo.local", "@bob:demo.local", "Bob",
       "Have you seen the new ncurses login screen? It supports Direct, Tor, I2P, Yggdrasil, and Custom proxy.");
    mk("$evt028", "!dm_bob:demo.local", "@demo:demo.local", "You",
       "Not yet! I'm testing through the HTTP API right now.");
    mk("$evt029", "!dm_bob:demo.local", "@bob:demo.local", "Bob",
       "Try `matrixcli tui` — it's the full terminal experience!");
}

std::vector<DemoMessage> DemoData::messagesForRoom(const std::string& room_id) const {
    std::vector<DemoMessage> result;
    for (const auto& m : _messages) {
        if (m.room_id == room_id) {
            result.push_back(m);
        }
    }
    return result;
}

const DemoRoom* DemoData::roomById(const std::string& room_id) const {
    for (const auto& r : _rooms) {
        if (r.id == room_id) return &r;
    }
    return nullptr;
}

void DemoData::addMessage(const DemoMessage& msg) {
    _messages.insert(_messages.begin(), msg);
}

json DemoData::roomsToJson(const std::string& format) const {
    json result = json::array();
    for (const auto& r : _rooms) {
        result.push_back(renderRoomJson(r));
    }
    return result;
}

json DemoData::messagesToJson(const std::string& room_id, int limit,
                                const std::string& format) const {
    auto msgs = messagesForRoom(room_id);
    json result = json::array();
    int count = 0;
    for (const auto& m : msgs) {
        if (count >= limit) break;
        result.push_back(renderMessageJson(m));
        count++;
    }
    return result;
}

json DemoData::statusToJson() const {
    return {
        {"logged_in", true},
        {"user_id", "@demo:demo.local"},
        {"device_id", "DEMODEVICE"},
        {"demo_mode", true},
        {"rooms_count", _rooms.size()},
        {"messages_count", _messages.size()}
    };
}

json DemoData::syncToJson() const {
    json rooms;
    for (const auto& r : _rooms) {
        auto msgs = messagesForRoom(r.id);
        json timeline = json::object();
        json events = json::array();
        int count = 0;
        for (const auto& m : msgs) {
            if (count >= 5) break;
            events.push_back(renderMessageJson(m));
            count++;
        }
        timeline["events"] = events;
        timeline["limited"] = (msgs.size() > 5);

        json room_data;
        room_data["timeline"] = timeline;
        rooms[r.id] = room_data;
    }

    json result;
    result["next_batch"] = "demo_sync_token_" + std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    result["rooms"] = {{"join", rooms}};
    return result;
}

json DemoData::renderRoomJson(const DemoRoom& r) {
    return {
        {"room_id", r.id},
        {"name", r.name},
        {"topic", r.topic},
        {"member_count", r.member_count},
        {"is_direct", r.is_direct}
    };
}

json DemoData::renderMessageJson(const DemoMessage& m) {
    json j = {
        {"event_id", m.event_id},
        {"room_id", m.room_id},
        {"sender", m.sender},
        {"origin_server_ts", m.timestamp},
        {"type", "m.room.message"},
        {"content", {
            {"body", m.body},
            {"msgtype", m.msgtype}
        }}
    };
    if (!m.formatted_body.empty()) {
        j["content"]["format"] = "org.matrix.custom.html";
        j["content"]["formatted_body"] = m.formatted_body;
    }
    return j;
}

std::string DemoData::renderRoomText(const DemoRoom& r) {
    std::ostringstream oss;
    oss << r.name << " (" << r.id << ")\n"
        << "  Members: " << r.member_count
        << (r.is_direct ? "  [DM]" : "") << "\n";
    if (!r.topic.empty()) oss << "  Topic: " << r.topic << "\n";
    return oss.str();
}

std::string DemoData::renderRoomMarkdown(const DemoRoom& r) {
    std::ostringstream oss;
    oss << "## " << r.name << "\n"
        << (r.is_direct ? "*Direct chat*\n\n" : "\n")
        << r.topic << "\n\n"
        << "`" << r.id << "` | " << r.member_count << " members\n";
    return oss.str();
}

std::string DemoData::renderMessageText(const DemoMessage& m) {
    std::ostringstream oss;
    oss << "[" << m.sender_name << "] " << m.body << "\n";
    return oss.str();
}

std::string DemoData::renderMessageMarkdown(const DemoMessage& m) {
    std::ostringstream oss;
    oss << "**" << m.sender_name << "**: " << m.body << "\n\n";
    return oss.str();
}

}} // namespace matrixcli::demo
