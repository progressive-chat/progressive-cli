// src/room_commands.cpp — session/room/API commands on the vendored desktop
// core (lib/ecore): one-shot sync, device management, moderation, profiles,
// threads, public room directory. Registered via CommandRegistry.
#include "commands.hpp"
#include "pcore.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "ascii_ui_impl.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/string_utils.hpp"
#include <progressive/markdown.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace matrixcli;

// Forward declarations for the commands defined in room_commands_discovery.cpp.
int cmdLink(const cli::Args& args);
int cmdTurn(const cli::Args& args);
int cmdOpenId(const cli::Args& args);
int cmdCapabilities(const cli::Args& args);
int cmdThirdparty(const cli::Args& args);

// Resolve a room id from a name/alias via the offline cache; falls back to
// the input as-is (room ids and aliases pass through).
static std::string resolveRoom(const std::string& query) {
    db::Database dbi;
    if (dbi.open("matrixcli.db")) {
        std::string id = matchRoomInCache(dbi.listRooms(), query);
        if (!id.empty()) return id;
    }
    return query;
}

int cmdSync(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    bool json_out = args.options.count("json");

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto roomsSeen = std::make_shared<std::atomic<int>>(0);
    auto eventsSeen = std::make_shared<std::atomic<int>>(0);

    // E2EE init (olm account + device keys) so encrypted timeline events can
    // be decrypted on the next serve pass; non-fatal on failure.
    std::string note = pcore::bootstrap();
    if (!note.empty() && !json_out) std::cerr << "Warning: " << note << std::endl;

    pcore::startSync([done, roomsSeen, eventsSeen](const progressive::desktop::FastSyncResponse& resp) {
        pcore::feedCache(resp);
        roomsSeen->fetch_add((int)resp.joinedRooms.size());
        eventsSeen->fetch_add(resp.totalTimelineEvents);
        done->store(true);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!done->load() && matrixcli::g_interrupted.load()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    pcore::stopSync();

    if (!done->load()) {
        if (!matrixcli::g_interrupted.load())
            std::cerr << "sync: no response within 120s" << std::endl;
        else
            std::cerr << "sync: interrupted (Ctrl+C)" << std::endl;
        return 1;
    }
    if (json_out) {
        nlohmann::json j;
        j["rooms"] = roomsSeen->load();
        j["events"] = eventsSeen->load();
        std::cout << j.dump() << std::endl;
    } else {
        std::cout << "Synced " << roomsSeen->load() << " rooms, "
                  << eventsSeen->load() << " timeline events into the cache." << std::endl;
    }
    return 0;
}

int cmdDevices(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty() || args.positional[0] != "delete" || args.positional.size() < 2) {
        std::cerr << "Usage: progressive-cli devices delete <deviceId> --password <pw>" << std::endl;
        return 1;
    }
    std::string password = args.options.count("password") ? args.options.at("password") : "";
    if (password.empty()) {
        std::cerr << "--password required (UIA challenge)" << std::endl;
        return 1;
    }
    std::string deviceId = args.positional[1];
    auto r = pcore::core().client->deleteDevice(deviceId, password);
    if (r.ok) {
        std::cout << "Device deleted: " << deviceId << std::endl;
        return 0;
    }
    std::string err = r.error.message.empty() ? "failed" : r.error.message;
    std::cerr << "Device deletion failed: " << err << std::endl;
    return 1;
}

int cmdModerate(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    std::string sub = args.command;  // kick | ban | unban
    if (args.positional.size() < 2) {
        std::cerr << "Usage: progressive-cli " << sub << " <room> <@user> [--reason r]" << std::endl;
        return 1;
    }
    std::string room = resolveRoom(args.positional[0]);
    std::string user = args.positional[1];
    std::string reason = args.options.count("reason") ? args.options.at("reason") : "";
    bool json_out = args.options.count("json");

    auto& client = pcore::core().client;
    bool ok = false;
    if (sub == "kick") ok = client->kickUser(room, user, reason).ok;
    else if (sub == "ban") ok = client->banUser(room, user, reason).ok;
    else if (sub == "unban") ok = client->unbanUser(room, user).ok;

    if (json_out) {
        nlohmann::json j; j["ok"] = ok; j["action"] = sub; j["user"] = user; j["room"] = room;
        std::cout << j.dump() << std::endl;
    } else {
        std::cout << (ok ? ("✓ " + sub + " " + user + " from " + room)
                         : (sub + " failed: " + user + " / " + room)) << std::endl;
    }
    return ok ? 0 : 1;
}

int cmdProfile(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli profile <@user:server>" << std::endl;
        return 1;
    }
    auto r = pcore::core().client->getUserProfile(args.positional[0]);
    if (!r.ok) {
        std::string err = r.error.message.empty() ? "failed" : r.error.message;
        std::cerr << "Profile lookup failed: " << err << std::endl;
        return 1;
    }
    if (args.options.count("json")) {
        std::cout << r.data << std::endl;
        return 0;
    }
    try {
        auto j = nlohmann::json::parse(r.data);
        std::cout << "User:  " << args.positional[0] << std::endl;
        std::cout << "Name:  " << j.value("displayname", "") << std::endl;
        std::cout << "Avatar:" << j.value("avatar_url", "") << std::endl;
    } catch (...) {
        std::cout << r.data << std::endl;
    }
    return 0;
}

int cmdMembers(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli members <room>" << std::endl;
        return 1;
    }
    std::string room = resolveRoom(args.positional[0]);
    auto r = pcore::core().client->getRoomMembers(room);
    if (!r.ok) {
        std::cerr << "Members lookup failed: " << (r.error.message.empty() ? "failed" : r.error.message) << std::endl;
        return 1;
    }
    if (args.options.count("json")) {
        std::cout << r.data << std::endl;
        return 0;
    }
    try {
        auto j = nlohmann::json::parse(r.data);
        int n = 0;
        for (auto& [userId, info] : j["chunk"].items()) {
            (void)userId;
            std::string uid = info.value("user_id", "");
            std::string name = info.value("displayname", "");
            std::string membership = info.value("membership", "");
            std::cout << "  " << uid << (name.empty() ? "" : " (" + name + ")") << " [" << membership << "]" << std::endl;
            n++;
        }
        std::cout << n << " members" << std::endl;
    } catch (...) {
        std::cout << r.data << std::endl;
    }
    return 0;
}

// Convenient power-level editor (ACL). All variants require a live session and
// PUT a (merged) m.room.power_levels state event. Reads the current content via
// the state endpoint, applies the change, and sends it back so the room stays
// consistent (same shape Element's "Roles & Permissions" UI produces).
int cmdPowerEdit(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: progressive-cli power <set|user|event|reset> <room> ...\n";
        return 1;
    }
    const std::string& sub = args.positional[0];
    const std::string& roomArg = args.positional[1];
    matrixcli::db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database" << std::endl; return 1; }
    std::string roomId = matrixcli::matchRoomInCache(dbi.listRooms(), roomArg);
    if (roomId.empty()) roomId = roomArg;
    auto& client = pcore::core().client;

    // Fetch current m.room.power_levels content (or start from empty).
    nlohmann::json pl = nlohmann::json::object();
    auto cur = client->getRoomStateEvent(roomId, "m.room.power_levels", "");
    if (cur.ok && !cur.data.empty()) {
        try { pl = nlohmann::json::parse(cur.data); } catch (...) { pl = nlohmann::json::object(); }
    }
    if (!pl.is_object()) pl = nlohmann::json::object();
    if (!pl.contains("users") || !pl["users"].is_object()) pl["users"] = nlohmann::json::object();
    if (!pl.contains("events") || !pl["events"].is_object()) pl["events"] = nlohmann::json::object();
    if (!pl.contains("notifications") || !pl["notifications"].is_object())
        pl["notifications"] = nlohmann::json::object();

    auto parseLevel = [](const std::string& s, int& out) -> bool {
        try { out = std::stoi(s); return true; } catch (...) { return false; }
    };

    if (sub == "reset") {
        pl = nlohmann::json::object({
            {"users_default", 0}, {"events_default", 0}, {"state_default", 50},
            {"ban", 50}, {"kick", 50}, {"invite", 0}, {"redact", 50},
            {"events", nlohmann::json::object({
                {"m.room.name", 50}, {"m.room.topic", 50}, {"m.room.avatar", 50},
                {"m.room.canonical_alias", 50}, {"m.room.history_visibility", 100},
                {"m.room.guest_access", 50}, {"m.room.encryption", 100},
                {"m.room.tombstone", 100}, {"m.room.server_acl", 100},
                {"m.room.third_party_invite", 50}, {"m.room.power_levels", 100},
                {"m.room.bridging", 50}, {"m.room.pinned_events", 50},
                {"m.room.message", 0}, {"m.room.encrypted", 0},
                {"m.reaction", 0}, {"m.sticker", 0}})},
            {"notifications", nlohmann::json::object({{"room", 50}})}
        });
        std::cout << "Resetting power levels to recommended defaults…" << std::endl;
    } else if (sub == "set") {
        if (args.positional.size() < 4) {
            std::cerr << "Usage: progressive-cli power set <room> <key> <level>\n"
                         "  keys: invite kick ban redact state_default events_default users_default notify\n";
            return 1;
        }
        int lvl;
        if (!parseLevel(args.positional[3], lvl)) { std::cerr << "level must be an integer\n"; return 1; }
        const std::string& key = args.positional[2];
        if (key == "notify") pl["notifications"]["room"] = lvl;
        else pl[key] = lvl;
        std::cout << "Setting " << key << " = " << lvl << "\n";
    } else if (sub == "user") {
        if (args.positional.size() < 4) {
            std::cerr << "Usage: progressive-cli power user <room> <@user> <level>\n";
            return 1;
        }
        int lvl;
        if (!parseLevel(args.positional[3], lvl)) { std::cerr << "level must be an integer\n"; return 1; }
        pl["users"][args.positional[2]] = lvl;
        std::cout << "Setting power level of " << args.positional[2] << " = " << lvl << "\n";
    } else if (sub == "event") {
        if (args.positional.size() < 4) {
            std::cerr << "Usage: progressive-cli power event <room> <type> <level>\n";
            return 1;
        }
        int lvl;
        if (!parseLevel(args.positional[3], lvl)) { std::cerr << "level must be an integer\n"; return 1; }
        pl["events"][args.positional[2]] = lvl;
        std::cout << "Setting required level for " << args.positional[2] << " = " << lvl << "\n";
    }

    auto res = client->sendStateEvent(roomId, "m.room.power_levels", "", pl.dump());
    if (!res.ok) {
        std::string err = res.error.message.empty() ? "failed" : res.error.message;
        std::cerr << "Power levels update failed: " << err << std::endl;
        return 1;
    }
    std::cout << "✓ power levels updated (event " << res.data << ")\n";
    return 0;
}

int cmdPower(const cli::Args& args) {
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli power <room>" << std::endl;
        return 1;
    }
    // Editing subcommands (power set/user/event/reset) PUT m.room.power_levels.
    const std::string& first = args.positional[0];
    if (first == "set" || first == "user" || first == "event" || first == "reset")
        return cmdPowerEdit(args);
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database" << std::endl; return 1; }
    std::string q = args.positional[0];
    std::string roomId = matchRoomInCache(dbi.listRooms(), q);
    if (roomId.empty()) roomId = q;
    std::string name = roomId;
    for (auto& r : dbi.listRooms())
        if (r.value("room_id", "") == roomId) { name = r.value("name", roomId); break; }
    // The latest m.room.power_levels state event (cached from sync; for the
    // demo it is inserted by populateDemoData). Offline — no network call.
    nlohmann::json pl;
    auto evs = dbi.getEvents(roomId, 100000);
    for (const auto& ev : evs)
        if (ev.type == "m.room.power_levels" && ev.content.is_object()) pl = ev.content;
    std::cout << ANSI_BOLD << name << ANSI_RESET << " — power levels\n";
    if (pl.is_null() || pl.empty()) {
        std::cout << "  (no m.room.power_levels state — everyone defaults to level 0)\n";
        return 0;
    }
    auto num = [&](const char* k, int def) -> int {
        return pl.contains(k) && pl[k].is_number() ? pl[k].get<int>() : def;
    };
    auto evLevel = [&](const std::string& type, int dflt) -> int {
        if (pl.contains("events") && pl["events"].is_object() &&
            pl["events"].contains(type) && pl["events"][type].is_number())
            return pl["events"][type].get<int>();
        return dflt;
    };
    int sd = num("state_default", 50);
    int ed = num("events_default", 0);
    std::cout << "  Defaults:\n";
    std::cout << "    users_default:  " << num("users_default", 0) << "\n";
    std::cout << "    events_default: " << ed << "  (send messages & most events)\n";
    std::cout << "    state_default:  " << sd << "  (change room settings)\n";
    // The four key-governed actions from the Matrix protocol. `redact` is
    // the right to delete (redact) any message — others' included.
    std::cout << "  Actions (required level):\n";
    std::cout << "    invite:                " << num("invite", 50) << "\n";
    std::cout << "    kick:                  " << num("kick", 50) << "\n";
    std::cout << "    ban:                   " << num("ban", 50) << "\n";
    std::cout << "    redact (delete msgs):  " << num("redact", 50) << "\n";
    // Sending content is governed by events_default (any event type not
    // listed under `events`). List the common client-facing ones with
    // their effective required level, like Element's "Send messages".
    static const char* kSendEvents[] = {
        "m.room.message", "m.room.encrypted", "m.reaction", "m.sticker", nullptr
    };
    std::cout << "  Sending messages & events (effective level):\n";
    for (int i = 0; kSendEvents[i]; ++i) {
        std::string lbl = kSendEvents[i];
        std::string pretty = lbl;
        if (lbl == "m.room.message")    pretty = "send message";
        if (lbl == "m.room.encrypted")  pretty = "send encrypted msg";
        if (lbl == "m.reaction")        pretty = "react";
        if (lbl == "m.sticker")         pretty = "sticker";
        std::cout << "    " << pretty << " (" << lbl << "): "
                  << evLevel(lbl, ed) << "\n";
    }
    // The full set of state events the protocol governs by power levels.
    // Anything not explicitly listed in `events` falls back to
    // state_default — show the effective required level for each.
    static const char* kStateEvents[] = {
        "m.room.name", "m.room.topic", "m.room.avatar",
        "m.room.canonical_alias", "m.room.history_visibility",
        "m.room.guest_access", "m.room.encryption", "m.room.tombstone",
        "m.room.server_acl", "m.room.third_party_invite",
        "m.room.power_levels", "m.room.bridging", "m.room.pinned_events",
        "im.vector.modular.widgets", nullptr
    };
    auto inKnown = [&](const std::string& k) {
        for (int i = 0; kStateEvents[i]; ++i) if (k == kStateEvents[i]) return true;
        return false;
    };
    std::cout << "  State events (effective required level):\n";
    for (int i = 0; kStateEvents[i]; ++i) {
        std::cout << "    " << kStateEvents[i] << ": " << evLevel(kStateEvents[i], sd) << "\n";
    }
    if (pl.contains("events") && pl["events"].is_object()) {
        bool any = false;
        for (auto it = pl["events"].begin(); it != pl["events"].end(); ++it) {
            if (inKnown(it.key())) continue;
            bool isSend = false;
            for (int i = 0; kSendEvents[i]; ++i) if (it.key() == kSendEvents[i]) isSend = true;
            if (isSend) continue;
            if (!any) { std::cout << "  Other event types:\n"; any = true; }
            int v = it.value().is_number() ? it.value().get<int>() : 0;
            std::cout << "    " << it.key() << ": " << v << "\n";
        }
    }
    if (pl.contains("notifications") && pl["notifications"].is_object()) {
        std::cout << "  Notifications:\n";
        for (auto it = pl["notifications"].begin(); it != pl["notifications"].end(); ++it) {
            int v = it.value().is_number() ? it.value().get<int>() : 0;
            std::cout << "    " << it.key() << ": " << v << "\n";
        }
    }
    if (pl.contains("users") && pl["users"].is_object()) {
        std::cout << "  Users:\n";
        std::vector<std::pair<int, std::string>> us;
        for (auto it = pl["users"].begin(); it != pl["users"].end(); ++it) {
            int v = it.value().is_number() ? it.value().get<int>() : 0;
            us.emplace_back(v, it.key());
        }
        std::sort(us.begin(), us.end(),
                  [](const std::pair<int, std::string>& a,
                     const std::pair<int, std::string>& b) { return a.first > b.first; });
        for (auto& [v, u] : us)
            std::cout << "    " << u << ": " << v
                      << (v >= 100 ? "  (admin)" : v >= 50 ? "  (mod)" : "") << "\n";
    }
    return 0;
}

int cmdThreads(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli threads <room> [--limit N]" << std::endl;
        return 1;
    }
    std::string room = resolveRoom(args.positional[0]);
    int limit = 20;
    if (args.options.count("limit")) { try { limit = std::stoi(args.options.at("limit")); } catch (...) {} }
    auto r = pcore::core().client->getThreads(room, "", limit);
    if (!r.ok) {
        std::cerr << "Threads lookup failed: " << (r.error.message.empty() ? "failed" : r.error.message) << std::endl;
        return 1;
    }
    std::cout << r.data << std::endl;
    return 0;
}

int cmdSearchPublic(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli search-public <query> [--server hs]" << std::endl;
        return 1;
    }
    std::string server = args.options.count("server") ? args.options.at("server") : "";
    auto r = pcore::core().client->searchPublicRooms(server, args.positional[0]);
    if (!r.ok) {
        std::cerr << "Directory lookup failed: " << (r.error.message.empty() ? "failed" : r.error.message) << std::endl;
        return 1;
    }
    if (args.options.count("json")) {
        std::cout << r.data << std::endl;
        return 0;
    }
    try {
        auto j = nlohmann::json::parse(r.data);
        int n = 0;
        for (auto& room : j["chunk"]) {
            std::cout << "  " << room.value("name", "?") << "  " << room.value("canonical_alias", "") << "  "
                      << room.value("num_joined_members", 0) << " members" << std::endl;
            n++;
        }
        std::cout << n << " rooms found" << std::endl;
    } catch (...) {
        std::cout << r.data << std::endl;
    }
    return 0;
}

// Render markdown to HTML with the vendored desktop renderer
// (progressive::markdownToHtml from lib/ecore/native/markdown.cpp).
//   matrixcli markdown <text>          — positional text
//   echo <text> | matrixcli markdown   — stdin pipe
//   --json — {html, input}; --no-tables/--no-links/--no-code/--no-scroll
int cmdMarkdown(const cli::Args& args) {
    progressive::MdConfig cfg;
    if (args.options.count("no-tables")) cfg.enableTables = false;
    if (args.options.count("no-links")) cfg.enableLinks = false;
    if (args.options.count("no-code")) cfg.enableCodeBlocks = false;
    if (args.options.count("no-scroll")) cfg.enableHorizontalScroll = false;
    bool json_out = args.options.count("json");

    std::string md;
    if (!args.positional.empty()) {
        for (size_t i = 0; i < args.positional.size(); i++) {
            if (i) md += " ";
            md += args.positional[i];
        }
    } else if (!isatty(STDIN_FILENO)) {
        std::string line;
        while (std::getline(std::cin, line)) { md += line; md += "\n"; }
    }
    if (md.empty()) {
        std::cerr << "Usage: progressive-cli markdown <text> | echo <text> | matrixcli markdown --json" << std::endl;
        return 1;
    }

    std::string html = progressive::markdownToHtml(md, cfg);
    if (json_out) {
        nlohmann::json j;
        j["html"] = html;
        j["input"] = md;
        std::cout << j.dump() << std::endl;
    } else {
        std::cout << html;
    }
    return 0;
}

// ── accounts ── list accounts the client is logged in as; hide/show them.
// Hidden accounts come from config.json "hidden_accounts" (permanent,
// undone via --show); --temporary-hide filters this invocation only.
int cmdAccounts(const cli::Args& args) {
    bool json_out = args.options.count("json");
    bool show_all = args.options.count("all");

    Config::instance().load("config.json");
    std::vector<std::string> hidden = Config::instance().hiddenAccounts();
    std::string cur_user = Config::instance().get("user_id");
    std::string cur_dev  = Config::instance().get("device_id");
    std::string cur_hs   = Config::instance().get("homeserver_url");
    std::string cur_tok  = Config::instance().get("access_token");

    // Accounts from the vendored SessionStore (one row per user) + the
    // config.json account (may not be persisted into session.db yet).
    pcore::init();   // ensure the session store is open (accounts needs no session)
    std::vector<progressive::desktop::AccountInfo> accounts;
    auto& core = pcore::core();
    if (core.storeOk) accounts = core.store->listAccounts();
    bool found_cur = false;
    for (auto& a : accounts) if (a.userId == cur_user && a.deviceId == cur_dev) { found_cur = true; break; }
    if (!found_cur && !cur_user.empty()) {
        progressive::desktop::AccountInfo a;
        a.userId = cur_user; a.deviceId = cur_dev; a.homeserverUrl = cur_hs; a.accessToken = cur_tok;
        accounts.push_back(a);
    }

    // --hide / --show (permanent, config.json) or --temporary-hide (this run)
    auto splitList = [](const std::string& csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    auto hide_it = args.options.find("hide");
    auto show_it = args.options.find("show");
    if (hide_it != args.options.end() || show_it != args.options.end()) {
        std::string mxid = hide_it != args.options.end() ? hide_it->second : show_it->second;
        if (mxid.empty()) { std::cerr << "Usage: accounts --hide <mxid> | --show <mxid>" << std::endl; return 1; }
        if (hide_it != args.options.end()) {
            if (std::find(hidden.begin(), hidden.end(), mxid) == hidden.end()) {
                hidden.push_back(mxid);
                Config::instance().setHiddenAccounts(hidden);
                Config::instance().save();
            }
            std::cout << "Account " << mxid << " hidden from the accounts list (undo: accounts --show "
                      << mxid << ")." << std::endl;
        } else {
            hidden.erase(std::remove(hidden.begin(), hidden.end(), mxid), hidden.end());
            Config::instance().setHiddenAccounts(hidden);
            Config::instance().save();
            std::cout << "Account " << mxid << " visible again." << std::endl;
        }
        return 0;
    }
    std::vector<std::string> tmp_hidden;
    auto th_it = args.options.find("temporary-hide");
    if (th_it != args.options.end()) tmp_hidden = splitList(th_it->second);

    auto isHidden = [&](const progressive::desktop::AccountInfo& a) {
        if (std::find(tmp_hidden.begin(), tmp_hidden.end(), a.userId) != tmp_hidden.end()) return true;
        return std::find(hidden.begin(), hidden.end(), a.userId) != hidden.end();
    };
    auto isActive = [&](const progressive::desktop::AccountInfo& a) {
        return !cur_user.empty() && a.userId == cur_user && (cur_dev.empty() || a.deviceId == cur_dev);
    };

    if (json_out) {
        nlohmann::json j = nlohmann::json::array();
        for (auto& a : accounts) {
            if (isHidden(a) && !show_all) continue;
            nlohmann::json e;
            e["user_id"] = a.userId;
            e["device_id"] = a.deviceId;
            e["homeserver_url"] = a.homeserverUrl;
            e["active"] = isActive(a);
            e["hidden"] = isHidden(a);
            j.push_back(e);
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    if (accounts.empty()) { std::cout << "No accounts. Run 'progressive-cli login'." << std::endl; return 0; }
    int shown = 0;
    for (auto& a : accounts) {
        if (isHidden(a) && !show_all) continue;
        shown++;
        std::string act = isActive(a) ? ANSI_GREEN " [active]" ANSI_RESET : "";
        std::string hid = isHidden(a) ? ANSI_DIM " (hidden)" ANSI_RESET : "";
        std::cout << "  " << a.userId << " (" << a.deviceId << ") @ " << a.homeserverUrl
                  << act << hid << std::endl;
    }
    if (shown == 0) std::cout << "(no visible accounts — accounts --all shows hidden ones)" << std::endl;
    return 0;
}


// ---- copy: the last N cached messages -> clipboard (OSC52 / wl-copy / xclip) ----
static std::string b64(const std::string& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    int v = 0, bits = -6;
    for (unsigned char c : in) {
        v = (v << 8) + c; bits += 8;
        while (bits >= 0) { o += T[(v >> bits) & 0x3F]; bits -= 6; }
    }
    if (bits > -6) o += T[((v << 8) >> (bits + 8)) & 0x3F];
    while (o.size() % 4) o += '=';
    return o;
}

int cmdCopy(const cli::Args& args) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open matrixcli.db\n"; return 1;
    }
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli copy <room> [N] [--preview]\n"
                     "Copies the last N cached messages as plain text"
                     " (default N=20).\n";
        return 1;
    }
    const std::string room = matchRoomInCache(dbi.listRooms(), args.positional[0]);
    if (room.empty()) { std::cerr << "Room not found in cache\n"; return 1; }

    int limit = 20;
    if (!args.positional.empty() && args.positional.size() > 1) {
        try { limit = std::max(1, std::stoi(args.positional[1])); } catch (...) {}
    }

    const auto evs = dbi.getEvents(room, limit);
    std::ostringstream out;
    for (const auto& ev : evs) {
        std::string sender = ev.sender;
        const auto at = sender.find(':');
        if (at != std::string::npos) sender = sender.substr(1, at - 1);
        out << sender << ": " << ev.content.value("body", "") << "\n";
    }
    const std::string payload = out.str();
    if (args.options.count("preview")) {
        std::cout << payload; return 0;
    }

    // Wayland first, then X11; OSC52 always as a last-mile fallback.
    bool via = false;
    if (std::getenv("WAYLAND_DISPLAY")) {
        FILE* p = popen("wl-copy 2>/dev/null", "w");
        if (p) { fwrite(payload.data(), 1, payload.size(), p); pclose(p); via = true; }
    } else if (std::system("command -v xclip >/dev/null 2>&1") == 0) {
        FILE* p = popen("xclip -selection clipboard -in 2>/dev/null", "w");
        if (p) { fwrite(payload.data(), 1, payload.size(), p); pclose(p); via = true; }
    }
#ifdef __unix__
    std::cout << "\033]52;c;" << b64(payload) << "\a";
#endif
    std::cout << "Copied " << evs.size() << " message(s) ("
              << (via ? "native clipboard" : "OSC 52") << ")\n";
    return 0;
}

// ---- dump: the room export from the cache (the ASCII UI parity) ----
int cmdDump(const cli::Args& args) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open matrixcli.db" << std::endl;
        return 1;
    }
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli dump <room> [--format json|txt|html|md]"
                     " [--out dir] [--limit N] [--parts N]"
                     " [--no-time: no timestamps (txt) / origin_server_ts (json)]\n"
                     "  The export reads the offline cache; the full server-side"
                     " pagination is the ASCII UI's 'dump --server'.\n";
        return 1;
    }
    const std::string room = resolveRoom(args.positional[0]);
    const std::string fmt = args.options.count("format") ? args.options.at("format") : "json";
    const std::string outDir = args.options.count("out") ? args.options.at("out") : ".";
    const bool noTime = args.options.count("no-time");
    int limit = 0;
    if (args.options.count("limit")) {
        try { limit = std::stoi(args.options.at("limit")); } catch (...) {}
    }
    int parts = 0;
    if (args.options.count("parts")) {
        try { parts = std::stoi(args.options.at("parts")); } catch (...) {}
    }
    auto evs = dbi.getEvents(room, limit > 0 ? limit : 100000);

    std::string name = room;
    for (const auto& r : dbi.listRooms()) {
        if (r.value("room_id", "") == room) {
            name = r.value("name", room);
            break;
        }
    }
    std::string fileBase = name;
    for (char& ch : fileBase) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
            ch = '_';
    }
    std::filesystem::create_directories(outDir);
    // The cache returns the newest first; reverse to chronological order.
    std::reverse(evs.begin(), evs.end());

    // Render a contiguous slice of events in the requested format.
    auto writeChunk = [&](std::ostream& out, const std::vector<matrix::Event>& chunk) {
        if (fmt == "json") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& ev : chunk) {
                nlohmann::json e = {{"event_id", ev.event_id},
                                    {"sender", ev.sender},
                                    {"type", ev.type}};
                // --no-time: drop origin_server_ts from the payload too.
                if (!noTime) e["origin_server_ts"] = ev.origin_server_ts;
                e["content"] = ev.content;
                arr.push_back(e);
            }
            out << arr.dump(2) << "\n";
        } else if (fmt == "txt") {
            for (const auto& ev : chunk) {
                std::string sender = ev.sender;
                const auto at = sender.find(':');
                if (at != std::string::npos) sender = sender.substr(1, at - 1);
                if (noTime) {
                    out << sender << ": "
                        << ev.content.value("body", "") << "\n";
                    continue;
                }
                std::time_t t = ev.origin_server_ts / 1000;
                char tbuf[24];
                std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M",
                              std::localtime(&t));
                out << tbuf << "  " << sender << ": "
                    << ev.content.value("body", "") << "\n";
            }
        } else if (fmt == "html") {
            out << "<!DOCTYPE html>\n<html><body>\n<h1>" << name << "</h1>\n<ul>\n";
            for (const auto& ev : chunk) {
                out << "  <li><b>" << ev.sender << "</b>: "
                    << ev.content.value("body", "") << "</li>\n";
            }
            out << "</ul>\n</body></html>\n";
        } else {  // md
            out << "# " << name << "\n\n";
            for (const auto& ev : chunk) {
                std::string sender = ev.sender;
                const auto at = sender.find(':');
                if (at != std::string::npos) sender = sender.substr(1, at - 1);
                out << "**" << sender << "**: " << ev.content.value("body", "")
                    << "\n\n";
            }
        }
    };

    if (parts <= 1) {
        const std::string path = outDir + "/" + fileBase + "." + fmt;
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            std::cerr << "Cannot write " << path << std::endl;
            return 1;
        }
        writeChunk(out, evs);
        std::cout << "Dumped " << evs.size() << " events to " << path << std::endl;
    } else {
        size_t total = evs.size();
        size_t per = (total + (size_t)parts - 1) / (size_t)parts;  // ceil
        size_t written = 0;
        int made = 0;
        for (int p = 0; p < parts && written < total; ++p) {
            size_t start = (size_t)p * per;
            size_t end = std::min(start + per, total);
            std::string pstr = (p + 1 < 10 ? "0" : "") + std::to_string(p + 1);
            const std::string path = outDir + "/" + fileBase + ".part" + pstr + "." + fmt;
            std::ofstream out(path, std::ios::trunc);
            if (!out) {
                std::cerr << "Cannot write " << path << std::endl;
                return 1;
            }
            std::vector<matrix::Event> chunk(evs.begin() + start, evs.begin() + end);
            writeChunk(out, chunk);
            written += (end - start);
            ++made;
            std::cout << "  part " << (p + 1) << ": " << (end - start)
                      << " events -> " << path << "\n";
        }
        std::cout << "Dumped " << total << " events into " << made << " part(s).\n";
    }
    return 0;
}

// ---- invite ----
int cmdInvite(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: progressive-cli invite <room> <@user> [--reason r]" << std::endl;
        return 1;
    }
    const std::string room = resolveRoom(args.positional[0]);
    const std::string user = args.positional[1];
    const bool ok = pcore::core().client->inviteUser(room, user).ok;
    std::cout << (ok ? ("\u2713 invited " + user + " to " + room)
                     : ("invite failed: " + user + " / " + room)) << std::endl;
    return ok ? 0 : 1;
}


void registerRoomCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("link", cmdLink,
                    "Room event permalink: link <room> [last|first|N|-N] [--via N] [--copy] [--clip] [--domain D] (no args: last active room; config 'link_domain'; 'link_via'=0 all; 'fit_link_to_terminal'=on)");
    reg.registerCli("permalink", cmdLink,
                    "Room event permalink (alias of link): permalink <room> [last|first|N|-N] [--via N] [--copy] [--clip] [--domain D] (no args: last active room; config 'link_domain'; 'link_via'=0 all; 'fit_link_to_terminal'=on)");
    reg.registerCli("sync", cmdSync, "One-shot sync into the offline cache");
    reg.registerCli("devices", cmdDevices, "Device management: devices delete <id> --password <pw>");
    reg.registerCli("kick", cmdModerate, "Kick a user: kick <room> <@user> [--reason r]");
    reg.registerCli("ban", cmdModerate, "Ban a user: ban <room> <@user> [--reason r]");
    reg.registerCli("unban", cmdModerate, "Unban a user: unban <room> <@user>");
    reg.registerCli("profile", cmdProfile, "Show a user profile: profile <@user>");
    reg.registerCli("members", cmdMembers, "List room members: members <room>");
    reg.registerCli("power", cmdPower, "Show room power levels / permissions: power <room>");
    reg.registerCli("perms", cmdPower, "Alias of power: perms <room>");
    reg.registerCli("threads", cmdThreads, "List room threads: threads <room> [--limit N]");
    reg.registerCli("search-public", cmdSearchPublic, "Search public room directory: search-public <query> [--server hs]");
    reg.registerCli("accounts", cmdAccounts, "List logged-in accounts: accounts [--all] [--json] | --hide <mxid> | --show <mxid> | --temporary-hide <mxid>");
    reg.registerCli("markdown", cmdMarkdown, "Render markdown to HTML: markdown <text> | echo <text> | progressive-cli markdown");
    reg.registerCli("dump", cmdDump, "Export a room from the cache: dump <room> [--format json|txt|html|md] [--out dir] [--limit N] [--parts N]");
    reg.registerCli("copy", cmdCopy, "Copy the last N cached messages to the clipboard: copy <room> [N] [--preview]");
    reg.registerCli("invite", cmdInvite, "Invite a user: invite <room> <@user> [--reason r]");
    reg.registerCli("turn", cmdTurn, "Show VoIP TURN credentials: turn");
    reg.registerCli("openid", cmdOpenId, "Request an OpenID token for the current user: openid");
    reg.registerCli("capabilities", cmdCapabilities, "Show server capabilities: capabilities");
    reg.registerCli("thirdparty", cmdThirdparty, "Third-party networks: thirdparty [protocol [users|locations]] [--network id]");
}
