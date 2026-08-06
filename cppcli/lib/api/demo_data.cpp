#include "demo_data.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include "../util/logger.hpp"

namespace matrixcli { namespace demo {

static int64_t now_ts() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

DemoData& DemoData::instance() {
    static DemoData inst;
    return inst;
}

DemoData::DemoData() {
    generate();
}

std::string DemoData::nextEventId(const std::string& prefix) {
    return prefix + std::to_string(++_nextId);
}

void DemoData::generate() {
    int64_t now = now_ts();
    int64_t t = now - 86400; // start 1 day ago

    auto tick = [&](int seconds = 60) { t += seconds; return t; };
    auto evt = [&](const std::string& prefix = "$evt") { return nextEventId(prefix); };

    // ── Users ──
    DemoMember alice("alice", "Alice", "https://matrix.org/avatar/alice.png", "join", 100);
    DemoMember bob("bob", "Bob", "https://matrix.org/avatar/bob.png", "join", 50);
    DemoMember charlie("charlie", "Charlie", "", "join", 50);
    DemoMember dave("dave", "Dave", "", "join", 0);
    DemoMember eve("eve", "Eve", "", "join", 0);
    DemoMember frank("frank", "Frank", "", "invite", 0);
    DemoMember admin("admin", "matrixcli-bot", "", "join", 100);
    DemoMember demo_user("demo", "You (demo)", "", "join", 50);
    DemoMember guest("guest", "GuestBot", "", "leave", 0);

    auto makeRoom = [&](const std::string& id, const std::string& name,
                         const std::string& topic, bool direct, bool enc,
                         std::vector<DemoMember> members,
                         std::vector<DemoStateEvent> state = {}) {
        DemoRoom r;
        r.id = id; r.name = name; r.topic = topic;
        r.is_direct = direct; r.is_encrypted = enc;
        r.members = std::move(members);
        r.state_events = std::move(state);
        // auto-add member state events
        int64_t ts = now - 86400;
        for (auto& m : r.members) {
            r.state_events.push_back({"m.room.member", m.user_id, {
                {"membership", m.membership},
                {"displayname", m.display_name},
                {"avatar_url", m.avatar_url}
            }, ts, "@roombot:demo.local"});
        }
        return r;
    };

    auto stateEvt = [&](const std::string& type, const std::string& key, json content,
                         const std::string& sender = "@roombot:demo.local") {
        return DemoStateEvent{type, key, content, t, sender};
    };

    // ── Demo Messages helper ──
    std::vector<DemoMessage> msgs;

    auto msg = [&](const std::string& room, const std::string& sender, const std::string& name,
                   const std::string& body, const std::string& mt = "m.text") -> DemoMessage& {
        auto ts = tick();
        auto& m = msgs.emplace_back();
        m.event_id = nextEventId();
        m.room_id = room;
        m.sender = sender;
        m.sender_name = name;
        m.body = body;
        m.msgtype = mt;
        m.timestamp = ts;
        return m;
    };

    auto img = [&](const std::string& room, const std::string& sender, const std::string& name,
                   const std::string& body, int w, int h) -> DemoMessage& {
        auto& m = msg(room, sender, name, body, "m.image");
        m.url = "mxc://demo.local/" + m.event_id;
        m.width = w; m.height = h;
        return m;
    };

    auto fileMsg = [&](const std::string& room, const std::string& sender, const std::string& name,
                       const std::string& body, const std::string& mime, int64_t sz) -> DemoMessage& {
        auto& m = msg(room, sender, name, body, "m.file");
        m.url = "mxc://demo.local/" + m.event_id;
        m.mimetype = mime; m.size = sz;
        return m;
    };

    auto encrypted = [&](const std::string& room, const std::string& sender, const std::string& name,
                          const std::string& body) -> DemoMessage& {
        auto& m = msg(room, sender, name, body, "m.text");
        m.is_encrypted = true;
        return m;
    };

    auto notice = [&](const std::string& room, const std::string& sender, const std::string& name,
                       const std::string& body) -> DemoMessage& {
        return msg(room, sender, name, body, "m.notice");
    };

    auto emote = [&](const std::string& room, const std::string& sender, const std::string& name,
                      const std::string& body) -> DemoMessage& {
        return msg(room, sender, name, body, "m.emote");
    };

    auto reaction = [&](const std::string& room, const std::string& sender, const std::string& name,
                         const std::string& relatesTo, const std::string& key) {
        auto& m = msgs.emplace_back();
        m.event_id = nextEventId("$reaction");
        m.room_id = room;
        m.sender = sender;
        m.sender_name = name;
        m.body = key;
        m.relates_to = relatesTo;
        m.relation_type = "m.annotation";
        m.timestamp = tick();
    };

    auto edit = [&](const std::string& room, const std::string& sender, const std::string& name,
                    const std::string& newBody, const std::string& relatesTo) {
        auto& m = msgs.emplace_back();
        m.event_id = nextEventId("$edit");
        m.room_id = room;
        m.sender = sender;
        m.sender_name = name;
        m.body = newBody;
        m.msgtype = "m.text";
        m.relates_to = relatesTo;
        m.relation_type = "m.replace";
        m.timestamp = tick();
    };

    // ═══════════════════════════════════════════
    // #general: main chat
    // ═══════════════════════════════════════════
    auto g = "!general:demo.local";
    auto m1 = msg(g, alice.user_id, alice.display_name,
        "Welcome everyone! This is a demo of matrixcli — a terminal Matrix client.");
    auto m2 = msg(g, bob.user_id, bob.display_name,
        "Hey Alice! The C++ rewrite is impressive. Raw sockets + OpenSSL for HTTP, no libcurl dependency.");
    reaction(g, charlie.user_id, charlie.display_name, m2.event_id, "👍");
    auto m3 = msg(g, alice.user_id, alice.display_name,
        "Thanks! It supports 5 output formats: JSON, text, Markdown, Gemini, and HTML. Try ?format=html on the API!");
    auto m4 = msg(g, charlie.user_id, charlie.display_name,
        "Does it support end-to-end encryption?");
    auto m5 = msg(g, alice.user_id, alice.display_name,
        "Yes! Full libolm bindings — Olm for 1-to-1 and Megolm for group encryption. Check the #dev room for details.");
    reaction(g, bob.user_id, bob.display_name, m5.event_id, "🔥");
    img(g, bob.user_id, bob.display_name, "Screenshot of matrixcli TUI in action", 1200, 800);
    auto m6 = msg(g, alice.user_id, alice.display_name,
        "Also check out the demo mode — start with `matrixcli demo` and access everything without a Matrix account.");
    edit(g, alice.user_id, alice.display_name,
        "Also check out the demo mode — start with `matrixcli demo` and test everything without a real Matrix account.",
        m6.event_id);
    auto m7 = msg(g, dave.user_id, dave.display_name,
        "Can I use it with Tor?");
    auto m8 = msg(g, bob.user_id, bob.display_name,
        "Absolutely! You can route through Tor, I2P, or Yggdrasil. Just select the connection type at login.");
    reaction(g, dave.user_id, dave.display_name, m8.event_id, "🙏");
    auto m9 = msg(g, alice.user_id, alice.display_name,
        "The TUI is built with ncurses — full terminal UI with room list, message view, and composer. Try `matrixcli tui`.");
    fileMsg(g, charlie.user_id, charlie.display_name, "matrixcli-api-spec.md", "text/markdown", 4521);
    auto m10 = msg(g, eve.user_id, eve.display_name,
        "Just joined! This looks amazing. Can't wait to try the Gemini protocol output.");
    auto m11 = msg(g, alice.user_id, alice.display_name,
        "Welcome Eve! The Gemini output is at ?format=gemini — pure gemtext, works great in Lagrange.");
    reaction(g, eve.user_id, eve.display_name, m11.event_id, "❤️");

    // ═══════════════════════════════════════════
    // #dev: development discussions
    // ═══════════════════════════════════════════
    auto dv = "!dev:demo.local";
    msg(dv, alice.user_id, alice.display_name,
        "The HTTP client is implemented with raw BSD sockets + OpenSSL — no external dependencies.");
    msg(dv, bob.user_id, bob.display_name,
        "Nice! For the TUI, ncurses handles the rendering. Anyone looked at Notcurses for better visuals?");
    msg(dv, charlie.user_id, charlie.display_name,
        "Notcurses supports images, video, and multi-threading. Could be a good upgrade path.");
    reaction(dv, alice.user_id, alice.display_name, msgs.back().event_id, "🤔");
    msg(dv, bob.user_id, bob.display_name,
        "The crypto layer (lib/e2ee/) is complete: OlmAccount, OlmSession, MegolmOutbound/Inbound, CryptoManager orchestrating them all.");
    auto dvEnc = encrypted(dv, charlie.user_id, charlie.display_name,
        "[This message is end-to-end encrypted. In the demo, we show it as plain text for readability.]");
    msg(dv, alice.user_id, alice.display_name,
        "The cmdspec system auto-generates TypeScript type declarations from Go code — we could do similar for C++.");
    notice(dv, admin.user_id, admin.display_name,
        "NOTICE: CI pipeline now builds cppcli on every push. Tests: 12/12 passing.");
    fileMsg(dv, bob.user_id, bob.display_name, "build.log", "text/plain", 12480);
    emote(dv, charlie.user_id, charlie.display_name, "pushes a commit at 3am");
    msg(dv, alice.user_id, alice.display_name,
        "Key architecture: lib/http (transport) → lib/matrix (protocol) → lib/api (REST server) → lib/formats (renderers). Clean separation.");
    reaction(dv, bob.user_id, bob.display_name, msgs.back().event_id, "💯");

    // ═══════════════════════════════════════════
    // #random: jokes and fun
    // ═══════════════════════════════════════════
    auto rm = "!random:demo.local";
    msg(rm, bob.user_id, bob.display_name, "Why did the C++ developer break up with the Java developer? Too many exceptions! 😄");
    reaction(rm, alice.user_id, alice.display_name, msgs.back().event_id, "😂");
    msg(rm, alice.user_id, alice.display_name, "A SQL query walks into a bar, joins two tables and asks... can I get a date?");
    reaction(rm, charlie.user_id, charlie.display_name, msgs.back().event_id, "🤣");
    msg(rm, charlie.user_id, charlie.display_name,
        "Programmer: I have a problem. I'll use threads.\nProblem solved!\n... Now I have tow prblmes.");
    msg(rm, eve.user_id, eve.display_name,
        "Why do Python programmers wear glasses? Because they don't see sharp.");
    reaction(rm, bob.user_id, bob.display_name, msgs.back().event_id, "🤓");
    img(rm, dave.user_id, dave.display_name, "dank-meme.png", 640, 480);

    // ═══════════════════════════════════════════
    // #announcements
    // ═══════════════════════════════════════════
    auto an = "!announcements:demo.local";
    msg(an, admin.user_id, admin.display_name,
        "📣 [ANN] matrixcli v0.1.0 released — First C++ release with full Matrix CS API, E2EE, ncurses TUI.");
    msg(an, admin.user_id, admin.display_name,
        "📣 [ANN] New feature: multi-format REST API — JSON, plain text, Markdown, Gemtext, HTML.");
    msg(an, admin.user_id, admin.display_name,
        "📣 [ANN] Security: TLS certificate pinning now available for all connections.");
    msg(an, admin.user_id, admin.display_name,
        "📣 [ANN] Demo mode released! Try `matrixcli demo` to explore without a Matrix account.");

    // ═══════════════════════════════════════════
    // #security: encrypted room demo
    // ═══════════════════════════════════════════
    auto sec = "!security:demo.local";
    auto sec1 = encrypted(sec, alice.user_id, alice.display_name,
        "This room is end-to-end encrypted. All messages are protected by Megolm.");
    auto sec2 = encrypted(sec, bob.user_id, bob.display_name,
        "I can confirm — the Olm session between our devices is established and verified.");
    auto sec3 = encrypted(sec, charlie.user_id, charlie.display_name,
        "My device isn't verified yet. Can someone send me the SAS verification emojis?");
    encrypted(sec, alice.user_id, alice.display_name,
        "Sure! Compare these emojis: 🐱 🏰 🌮 🔑 🎸 🌵 — matching?");
    reaction(sec, charlie.user_id, charlie.display_name, sec3.event_id, "✅");
    encrypted(sec, charlie.user_id, charlie.display_name,
        "Verified! The emojis match. My device is now cross-signed.");
    msg(sec, admin.user_id, admin.display_name,
        "[SYSTEM] Device @charlie:demo.local (CHARLIEPHONE) is now verified");

    // ═══════════════════════════════════════════
    // DMs
    // ═══════════════════════════════════════════
    auto dmA = "!dm_alice:demo.local";
    encrypted(dmA, alice.user_id, alice.display_name,
        "Hey! This is a private encrypted DM. Only we can read these messages.");
    msg(dmA, demo_user.user_id, demo_user.display_name,
        "Cool! So the Olm session is established between our devices?");
    encrypted(dmA, alice.user_id, alice.display_name,
        "Exactly. Each device gets its own Olm session. Megolm is used for group chats.");
    msg(dmA, demo_user.user_id, demo_user.display_name,
        "And the keys are stored in SQLite for offline access?");
    encrypted(dmA, alice.user_id, alice.display_name,
        "Yes! The database caches events, sync tokens, and crypto keys — everything works offline.");
    reaction(dmA, alice.user_id, alice.display_name, msgs.back().event_id, "👌");

    auto dmB = "!dm_bob:demo.local";
    msg(dmB, bob.user_id, bob.display_name,
        "Have you seen the new ncurses login screen? It supports Direct, Tor, I2P, Yggdrasil, Custom.");
    msg(dmB, demo_user.user_id, demo_user.display_name,
        "Not yet! I'm testing through the HTTP API right now.");
    msg(dmB, bob.user_id, bob.display_name,
        "Try `matrixcli tui` — it's the full terminal experience with room switching and message composer.");
    img(dmB, bob.user_id, bob.display_name, "tui-screenshot.png", 1920, 1080);
    reaction(dmB, demo_user.user_id, demo_user.display_name, msgs.back().event_id, "🔥");

    _messages = std::move(msgs);

    // ── Rooms ──
    _rooms = {
        makeRoom(g, "#general", "General discussion — welcome to matrixcli demo!",
                 false, false, {alice, bob, charlie, dave, eve, demo_user},
                 {stateEvt("m.room.topic", "", {{"topic","General discussion — welcome to matrixcli demo!"}}),
                  stateEvt("m.room.name", "", {{"name","#general"}}),
                  stateEvt("m.room.join_rules", "", {{"join_rule","public"}})}),
        makeRoom(dv, "#dev", "Development chat — C++, Matrix protocol, ncurses, E2EE",
                 false, false, {alice, bob, charlie, admin, demo_user},
                 {stateEvt("m.room.topic", "", {{"topic","Development chat — C++, Matrix protocol, ncurses, E2EE"}}),
                  stateEvt("m.room.name", "", {{"name","#dev"}}),
                  stateEvt("m.room.join_rules", "", {{"join_rule","public"}})}),
        makeRoom(rm, "#random", "Random stuff and memes",
                 false, false, {alice, bob, charlie, eve, dave, demo_user},
                 {stateEvt("m.room.topic", "", {{"topic","Random stuff and memes"}})}),
        makeRoom(an, "#announcements", "Official announcements from the matrixcli team",
                 false, false, {admin, alice, bob, charlie, demo_user},
                 {stateEvt("m.room.topic", "", {{"topic","Official announcements from the matrixcli team"}}),
                  stateEvt("m.room.join_rules", "", {{"join_rule","public"}})}),
        makeRoom(sec, "#security", "E2EE testing room — encrypted",
                 false, true, {alice, bob, charlie, admin, demo_user},
                 {stateEvt("m.room.topic", "", {{"topic","E2EE testing room — encrypted"}}),
                  stateEvt("m.room.encryption", "", {{"algorithm","m.megolm.v1.aes-sha2"}})}),
        makeRoom(dmA, "Alice", "", true, true, {alice, demo_user}),
        makeRoom(dmB, "Bob", "", true, false, {bob, demo_user}),
    };

    // ── Devices ──
    _devices = {
        {"DEMODEVICE", "matrixcli demo instance", "127.0.0.1", now - 60, true},
        {"ALICEPHONE", "Element Android", "10.0.1.42", now - 300, true},
        {"ALICELAPTOP", "Element Desktop", "10.0.1.10", now - 7200, true},
        {"BOBWEB", "Element Web", "192.168.1.5", now - 600, false},
        {"CHARLIEPHONE", "FluffyChat", "172.16.0.3", now - 3600, false},
    };
}

DemoRoom* DemoData::roomById(const std::string& room_id) {
    for (auto& r : _rooms) if (r.id == room_id) return &r;
    return nullptr;
}

const DemoRoom* DemoData::roomById(const std::string& room_id) const {
    for (const auto& r : _rooms) if (r.id == room_id) return &r;
    return nullptr;
}

std::vector<DemoMessage> DemoData::messagesForRoom(const std::string& room_id,
                                                    const std::string& from,
                                                    const std::string& before,
                                                    int limit) const {
    std::vector<DemoMessage> filtered;
    for (const auto& m : _messages) {
        if (m.room_id != room_id) continue;
        filtered.push_back(m);
    }
    // messages are in chronological order (oldest first in the data),
    // but we want newest first for API
    std::reverse(filtered.begin(), filtered.end());

    // Filter by from (return messages after this event_id)
    if (!from.empty()) {
        auto it = std::find_if(filtered.begin(), filtered.end(),
            [&](const DemoMessage& m) { return m.event_id == from; });
        if (it != filtered.end()) {
            filtered.erase(it, filtered.end());
        }
    }
    // Filter by before (return messages before this event_id)
    if (!before.empty()) {
        auto it = std::find_if(filtered.begin(), filtered.end(),
            [&](const DemoMessage& m) { return m.event_id == before; });
        if (it != filtered.end()) {
            filtered.erase(filtered.begin(), it + 1);
        }
    }

    if (limit > 0 && limit < (int)filtered.size()) {
        filtered.resize(limit);
    }
    return filtered;
}

void DemoData::addMessage(const DemoMessage& msg) {
    _messages.insert(_messages.begin(), msg);
}

bool DemoData::save(const std::string& path) {
    try {
        json j;
        j["messages"] = json::array();
        for (auto& m : _messages) {
            auto msgJson = renderMessageJson(m);
            msgJson["sender_name"] = m.sender_name;
            msgJson["is_encrypted"] = m.is_encrypted;
            msgJson["relates_to"] = m.relates_to;
            msgJson["relation_type"] = m.relation_type;
            j["messages"].push_back(msgJson);
        }
        j["saved_at"] = now_ts();

        std::ofstream ofs(path);
        if (!ofs) return false;
        ofs << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool DemoData::load(const std::string& path) {
    try {
        std::ifstream ifs(path);
        if (!ifs) return false;
        json j = json::parse(ifs);
        if (!j.contains("messages") || !j["messages"].is_array()) return false;

        _messages.clear();
        for (auto& msg_json : j["messages"]) {
            DemoMessage m;
            m.event_id = msg_json.value("event_id", "");
            m.room_id = msg_json.value("room_id", "");
            m.sender = msg_json.value("sender", "");
            m.sender_name = msg_json.value("sender_name", "");
            m.timestamp = msg_json.value("origin_server_ts", (int64_t)0);
            m.is_encrypted = msg_json.value("is_encrypted", false);
            m.relates_to = msg_json.value("relates_to", "");
            m.relation_type = msg_json.value("relation_type", "");

            std::string evt_type = msg_json.value("type", "");
            auto ct = msg_json.value("content", json::object());
            m.msgtype = ct.value("msgtype", "m.text");
            m.body = ct.value("body", "");
            m.url = ct.value("url", "");

            if (evt_type == "m.room.encrypted") m.is_encrypted = true;
            else if (m.msgtype == "m.image" || m.msgtype == "m.video") {
                auto info = ct.value("info", json::object());
                m.width = info.value("w", 0);
                m.height = info.value("h", 0);
            } else if (m.msgtype == "m.file" || m.msgtype == "m.audio") {
                auto info = ct.value("info", json::object());
                m.mimetype = info.value("mimetype", "");
                m.size = info.value("size", (int64_t)0);
            }

            // Reactions
            if (ct.contains("m.relates_to")) {
                auto& rel = ct["m.relates_to"];
                m.relates_to = rel.value("event_id", "");
                m.relation_type = rel.value("rel_type", "");
                m.body = rel.value("key", "");
            }

            _messages.push_back(m);
        }

        if (j.contains("saved_at")) {
            util::Logger::instance().info(
                "Loaded " + std::to_string(_messages.size()) + " messages from " + path +
                " (saved at " + std::to_string(j["saved_at"].get<int64_t>()) + ")");
        } else {
            util::Logger::instance().info(
                "Loaded " + std::to_string(_messages.size()) + " messages from " + path);
        }
        return true;
    } catch (const std::exception& e) {
        util::Logger::instance().warn(std::string("Failed to load demo data: ") + e.what());
        return false;
    }
}

// ── JSON renderers ──

json DemoData::renderRoomJson(const DemoRoom& r) {
    json j = {
        {"room_id", r.id},
        {"name", r.name},
        {"topic", r.topic},
        {"is_direct", r.is_direct},
        {"is_encrypted", r.is_encrypted},
        {"member_count", r.members.size()}
    };
    if (!r.room_alias.empty()) j["canonical_alias"] = r.room_alias;
    if (!r.avatar_url.empty()) j["avatar_url"] = r.avatar_url;
    return j;
}

json DemoData::renderMessageJson(const DemoMessage& m) {
    json j;
    j["event_id"] = m.event_id;
    j["room_id"] = m.room_id;
    j["sender"] = m.sender;
    j["origin_server_ts"] = m.timestamp;

    if (m.is_encrypted) {
        j["type"] = "m.room.encrypted";
        j["content"] = {
            {"algorithm", "m.megolm.v1.aes-sha2"},
            {"sender_key", "demo_sender_key"},
            {"session_id", "demo_session_" + m.event_id},
            {"ciphertext", "[demo encrypted payload — " + m.body + "]"},
            {"device_id", "DEMODEVICE"}
        };
    } else if (!m.relates_to.empty()) {
        j["type"] = "m.reaction";
        j["content"] = {
            {"m.relates_to", {
                {"event_id", m.relates_to},
                {"rel_type", m.relation_type}
            }}
        };
        if (m.relation_type == "m.annotation") {
            j["content"]["m.relates_to"]["key"] = m.body.empty() ? "👍" : m.body;
        } else if (m.relation_type == "m.replace") {
            j["content"]["body"] = m.body;
            j["content"]["msgtype"] = m.msgtype;
            j["type"] = m.msgtype.empty() ? "m.room.message" : m.msgtype;
        }
    } else {
        j["type"] = m.msgtype.empty() ? "m.room.message" : m.msgtype;
        j["content"]["body"] = m.body;
        if (!m.msgtype.empty() && m.msgtype != "m.reaction") j["content"]["msgtype"] = m.msgtype;
        if (!m.formatted_body.empty()) {
            j["content"]["format"] = "org.matrix.custom.html";
            j["content"]["formatted_body"] = m.formatted_body;
        }
        if (!m.url.empty()) j["content"]["url"] = m.url;
        if (!m.mimetype.empty()) {
            j["content"]["info"] = {{"mimetype", m.mimetype}, {"size", m.size}};
        }
        if (m.width > 0) {
            if (!j["content"].contains("info")) j["content"]["info"] = json::object();
            j["content"]["info"]["w"] = m.width;
            j["content"]["info"]["h"] = m.height;
        }
    }

    return j;
}

json DemoData::renderMemberJson(const DemoMember& m) {
    return {
        {"user_id", m.user_id},
        {"display_name", m.display_name},
        {"avatar_url", m.avatar_url},
        {"membership", m.membership},
        {"power_level", m.power_level}
    };
}

json DemoData::renderStateEventJson(const DemoStateEvent& e) {
    return {
        {"type", e.type},
        {"state_key", e.state_key},
        {"sender", e.sender},
        {"origin_server_ts", e.timestamp},
        {"content", e.content}
    };
}

json DemoData::renderDeviceJson(const DemoDevice& d) {
    return {
        {"device_id", d.device_id},
        {"display_name", d.display_name},
        {"last_seen_ip", d.last_seen_ip},
        {"last_seen_ts", d.last_seen_ts},
        {"verified", d.verified}
    };
}

// ── Text renderers ──

std::string DemoData::renderRoomText(const DemoRoom& r) {
    std::ostringstream oss;
    oss << r.name << " (" << r.id << ")\n"
        << "  Members: " << r.members.size()
        << (r.is_direct ? "  [DM]" : "")
        << (r.is_encrypted ? "  [🔒 E2EE]" : "") << "\n";
    if (!r.topic.empty()) oss << "  Topic: " << r.topic << "\n";
    return oss.str();
}

std::string DemoData::renderMessageText(const DemoMessage& m) {
    std::ostringstream oss;
    if (m.is_encrypted) {
        oss << "[🔒] [" << m.sender_name << "] (encrypted)\n";
    } else if (!m.relates_to.empty()) {
        if (m.relation_type == "m.annotation") {
            oss << "[" << m.sender_name << "] reacted with " << (m.body.empty() ? "👍" : m.body)
                << " to " << m.relates_to << "\n";
        } else {
            oss << "[" << m.sender_name << "] (edited) " << m.body << "\n";
        }
    } else {
        const char* prefix = "";
        if (m.msgtype == "m.emote") prefix = "* ";
        else if (m.msgtype == "m.notice") prefix = "[NOTICE] ";
        oss << "[" << m.sender_name << "] " << prefix << m.body;
        if (m.msgtype == "m.image") oss << " [image: " << m.width << "x" << m.height << "]";
        else if (m.msgtype == "m.file") oss << " [file: " << m.mimetype << ", " << m.size << "B]";
        else if (m.msgtype == "m.video") oss << " [video: " << m.width << "x" << m.height << "]";
        oss << "\n";
    }
    return oss.str();
}

std::string DemoData::renderMemberText(const DemoMember& m) {
    std::ostringstream oss;
    oss << m.display_name << " (" << m.user_id << ")";
    if (m.power_level >= 100) oss << " [admin]";
    else if (m.power_level >= 50) oss << " [mod]";
    oss << " - " << m.membership << "\n";
    return oss.str();
}

std::string DemoData::renderDeviceText(const DemoDevice& d) {
    std::ostringstream oss;
    oss << d.display_name << " (" << d.device_id << ")\n"
        << "  IP: " << d.last_seen_ip
        << "  Last seen: " << d.last_seen_ts
        << (d.verified ? "  [VERIFIED]" : "  [UNVERIFIED]") << "\n";
    return oss.str();
}

// ── Markdown renderers ──

std::string DemoData::renderRoomMarkdown(const DemoRoom& r) {
    std::ostringstream oss;
    oss << "## " << r.name << "\n"
        << (r.is_direct ? "*Direct chat*  " : "")
        << (r.is_encrypted ? "*🔒 E2EE*  " : "")
        << "\n"
        << r.topic << "\n\n"
        << "`" << r.id << "` | " << r.members.size() << " members\n";
    return oss.str();
}

std::string DemoData::renderMessageMarkdown(const DemoMessage& m) {
    std::ostringstream oss;
    if (m.is_encrypted) {
        oss << "**" << m.sender_name << "** 🔒: *(encrypted)*\n\n";
    } else if (!m.relates_to.empty()) {
        if (m.relation_type == "m.annotation") {
            oss << "*" << m.sender_name << " reacted with "
                << (m.body.empty() ? "👍" : m.body) << "*\n\n";
        } else {
            oss << "**" << m.sender_name << "** (edited): " << m.body << "\n\n";
        }
    } else {
        if (m.msgtype == "m.emote") {
            oss << "*" << m.sender_name << " " << m.body << "*\n\n";
        } else {
            oss << "**" << m.sender_name << "**: " << m.body;
            if (m.msgtype == "m.image") oss << " *(image " << m.width << "×" << m.height << ")*";
            else if (m.msgtype == "m.file") oss << " *(file: " << m.mimetype << ")*";
            oss << "\n\n";
        }
    }
    return oss.str();
}

}} // namespace matrixcli::demo
