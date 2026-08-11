// src/room_commands.cpp — session/room/API commands on the vendored desktop
// core (lib/ecore): one-shot sync, device management, moderation, profiles,
// threads, public room directory. Registered via CommandRegistry.
#include "commands.hpp"
#include "pcore.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/string_utils.hpp"
#include <progressive/markdown.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace matrixcli;

// Resolve a room id from a name/alias via the offline cache; falls back to
// the input as-is (room ids and aliases pass through).
static std::string resolveRoom(const std::string& query) {
    db::Database dbi;
    if (dbi.open("matrixcli.db")) {
        for (auto& r : dbi.listRooms()) {
            std::string id = r.value("room_id", "");
            std::string name = r.value("name", "");
            if (id == query || name == query || name.find(query) == 0) return id;
        }
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
        std::cerr << "Usage: matrixcli devices delete <deviceId> --password <pw>" << std::endl;
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
        std::cerr << "Usage: matrixcli " << sub << " <room> <@user> [--reason r]" << std::endl;
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
        std::cerr << "Usage: matrixcli profile <@user:server>" << std::endl;
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
        std::cerr << "Usage: matrixcli members <room>" << std::endl;
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

int cmdThreads(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli threads <room> [--limit N]" << std::endl;
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
        std::cerr << "Usage: matrixcli search-public <query> [--server hs]" << std::endl;
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
        std::cerr << "Usage: matrixcli markdown <text> | echo <text> | matrixcli markdown --json" << std::endl;
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

    if (accounts.empty()) { std::cout << "No accounts. Run 'matrixcli login'." << std::endl; return 0; }
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

void registerRoomCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("sync", cmdSync, "One-shot sync into the offline cache");
    reg.registerCli("devices", cmdDevices, "Device management: devices delete <id> --password <pw>");
    reg.registerCli("kick", cmdModerate, "Kick a user: kick <room> <@user> [--reason r]");
    reg.registerCli("ban", cmdModerate, "Ban a user: ban <room> <@user> [--reason r]");
    reg.registerCli("unban", cmdModerate, "Unban a user: unban <room> <@user>");
    reg.registerCli("profile", cmdProfile, "Show a user profile: profile <@user>");
    reg.registerCli("members", cmdMembers, "List room members: members <room>");
    reg.registerCli("threads", cmdThreads, "List room threads: threads <room> [--limit N]");
    reg.registerCli("search-public", cmdSearchPublic, "Search public room directory: search-public <query> [--server hs]");
    reg.registerCli("accounts", cmdAccounts, "List logged-in accounts: accounts [--all] [--json] | --hide <mxid> | --show <mxid> | --temporary-hide <mxid>");
    reg.registerCli("markdown", cmdMarkdown, "Render markdown to HTML: markdown <text> | echo <text> | matrixcli markdown");
}
