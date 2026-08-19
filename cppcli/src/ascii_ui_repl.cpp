// src/ascii_ui.cpp — ASCII-drawn client interface for the CLI (not the TUI).
//
// `progressive-cli ui` draws a chat-client-like layout with plain characters:
// a header, a left panel with the room list, the open room's messages in
// the center and the member list on the right, panels separated by pipes.
// It is a REPL: every command executes and the whole frame is redrawn —
// no auto-updates, no raw terminal mode (works in any terminal, scrolls
// like a normal CLI program).
#include "ascii_ui.hpp"
#include "ascii_state.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/string_utils.hpp"
#include "agent_tools.hpp"
#include <cstdlib>
#include <glob.h>
#include <poll.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "cli/args.hpp"
#include "pcore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "ascii_ui_impl.hpp"

namespace matrixcli {

int cmdAsciiUi(const cli::Args& args) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open matrixcli.db — run 'progressive-cli demo populate' first"
                  << std::endl;
        return 1;
    }

    UiState st;
    st.db = &dbi;
    st.rooms = dbi.listRooms();
    sortRoomsByActivity(st);
    if (st.rooms.empty()) {
        std::cerr << "No rooms cached. Run 'progressive-cli demo populate' or sync first."
                  << std::endl;
        return 1;
    }
    // Who are we? The saved session's user id, or "demo (offline)".
    if (pcore::init() && pcore::loadSavedSession()) {
        std::string uid = pcore::core().client->account().userId;
        if (!uid.empty()) {
            if (uid[0] == '@') uid = uid.substr(1);
            st.accountLabel = uid;  // e.g. "bob@matrix.org"
        }
    }
    if (st.accountLabel.empty()) st.accountLabel = "demo (offline)";
    st.proxyLabel = proxyLabelText();
    // Invites count for the header: the demo user is "@you", sessions use
    // the saved mxid ("@" stripped in accountLabel).
    st.invites = dbi.inviteCount(st.accountLabel == "demo (offline)"
                                     ? "@you" : "@" + st.accountLabel);
    for (const auto& id : dbi.invitedRoomIds(st.accountLabel == "demo (offline)"
                                                 ? "@you" : "@" + st.accountLabel)) {
        st.invited.insert(id);
    }
    // The remembered invitation dates (the cache keeps the invite events
    // with their timestamps).
    for (const auto& inv : dbi.openInvites(st.accountLabel == "demo (offline)"
                                               ? "@you" : "@" + st.accountLabel)) {
        st.inviteByRoom[inv.roomId] = inv;
    }
    // Persisted settings (the Settings screen): restore them on start.
    st.showSeconds = dbi.getSetting("time_full") == "1";
    st.showIds = dbi.getSetting("ids") == "1";
    st.showImages = dbi.getSetting("images") == "1";
    st.showEmoji = dbi.getSetting("emoji") != "0";
    st.showInvites = dbi.getSetting("show_invites", "1") != "0";
    st.showInvitesLegend = dbi.getSetting("show_invites_legend", "1") != "0";
    st.showNotifications = dbi.getSetting("show_notifications", "1") != "0";
    st.showRoomInviteMark = dbi.getSetting("room_invite_mark", "1") != "0";
    st.limitRows = std::max(0, std::atoi(dbi.getSetting("rows", "0").c_str()));
    try { st.leftPanelW = std::stoi(dbi.getSetting("panel_left", "-1")); } catch (...) {}
    try { st.rightPanelW = std::stoi(dbi.getSetting("panel_right", "-1")); } catch (...) {}
    st.mobile = dbi.getSetting("mobile") == "1";
    // Auto: a narrow terminal (< 60 columns) cannot fit the three
    // columns, so the smartphone layout kicks in by itself.
    if (terminalWidthImpl() < 60) {
        st.mobile = true;
        st.statusNote = "narrow terminal · smartphone layout auto-enabled";
    }
    st.showNames = dbi.getSetting("names") != "0";
    st.showReceipts = dbi.getSetting("receipts") != "0";
    st.hiddenReceiptUsers.clear();
    {
        // "receipts hide <user>" — the comma-separated list of users whose
        // ✓ readers never show.
        std::string rh = dbi.getSetting("receipts_hide", "");
        size_t pos = 0;
        while (pos <= rh.size()) {
            size_t comma = rh.find(',', pos);
            std::string tok = rh.substr(pos, comma == std::string::npos
                                             ? std::string::npos : comma - pos);
            if (!tok.empty()) st.hiddenReceiptUsers.push_back(tok);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    st.showJoins = dbi.getSetting("joins") != "0";
    st.showLinks = dbi.getSetting("links") != "0";
    st.clock12h = dbi.getSetting("clock12h") == "1";
    st.timeRight = dbi.getSetting("time_side") == "right";
    st.msgNewline = dbi.getSetting("msg_line") == "newline";
    st.autoPanels = dbi.getSetting("panel_auto") != "0";
    try { st.membersMode = std::stoi(dbi.getSetting("members_mode", "0")); } catch (...) {}
    st.showThreadsBottom = dbi.getSetting("threads_bottom") != "0";
    try { st.viaLimit = std::stoi(dbi.getSetting("via_limit", "3")); } catch (...) {}
    try { st.tzOffset = std::stoi(dbi.getSetting("tz_offset", "0")); } catch (...) {}
    try { st.hiddenSeconds = std::stoi(dbi.getSetting("hidden_seconds", "12")); } catch (...) {}
    auto loadMap = [&](const std::string& key, std::map<std::string, std::string>& out) {
        std::string v = dbi.getSetting(key, "");
        std::string cur;
        for (char ch : v) {
            if (ch == ',') {
                auto eq = cur.find('=');
                if (eq != std::string::npos) out[cur.substr(0, eq)] = cur.substr(eq + 1);
                cur.clear();
            } else cur += ch;
        }
        auto eq = cur.find('=');
        if (eq != std::string::npos) out[cur.substr(0, eq)] = cur.substr(eq + 1);
    };
    loadMap("room_nicks", st.roomNicks);
    loadMap("room_avatars", st.roomAvatars);
    loadMap("user_colors", st.userColors);
    {
        std::string v = dbi.getSetting("muted", "");
        std::string cur;
        for (char ch : v) { if (ch == ',') { st.mutedRooms.insert(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) st.mutedRooms.insert(cur);
    }
    {
        std::string v = dbi.getSetting("starred", "");
        std::string cur;
        for (char ch : v) { if (ch == ',') { st.starredRooms.insert(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) st.starredRooms.insert(cur);
    }
    // The starred rooms must be known before the list is sorted.
    sortRoomsByActivity(st);
    // Custom character widths ("widths" setting): "cp:width,cp:width".
    {
        g_widthOverrides.clear();
        auto parseOne = [](const std::string& seg) {
            auto colon = seg.find(':');
            if (colon == std::string::npos) return;
            uint32_t cp = static_cast<uint32_t>(
                std::strtoul(seg.substr(0, colon).c_str(), nullptr, 16));
            int wd = std::atoi(seg.substr(colon + 1).c_str());
            if (cp > 0 && (wd == 1 || wd == 2)) {
                g_widthOverrides[cp] = wd;
            }
        };
        std::string w = dbi.getSetting("widths", "");
        std::string cur;
        for (char ch : w) {
            if (ch == ',') {
                parseOne(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        parseOne(cur);  // the last (or only) entry
    }
    // Demo/offline: static presence so the right panel shows the letters.
    // The demo events carry short senders ("@alice") — key both forms.
    if (st.accountLabel == "demo (offline)") {
        struct { const char* full; const char* short_; const char* letter; } demoPres[] = {
            {"@alice:demo.local", "@alice", "O"},
            {"@bob:demo.local", "@bob", "A"},
            {"@charlie:demo.local", "@charlie", "F"},
            {"@you:demo.local", "@you", "O"},
            {"@carol:demo.local", "@carol", "O"},
            {"@dave:demo.local", "@dave", "O"},
            {"@erin:demo.local", "@erin", "A"},
            {"@frank:demo.local", "@frank", "F"},
            {"@grace:demo.local", "@grace", "O"},
            {"@heidi:demo.local", "@heidi", "O"},
            {"@ivan:demo.local", "@ivan", "A"},
            {"@julia:demo.local", "@julia", "O"},
            {"@kate:demo.local", "@kate", "F"},
        };
        for (auto& p : demoPres) {
            st.presence[p.full] = p.letter;
            st.presence[p.short_] = p.letter;
        }
    }
    std::string initial = args.positional.empty() ? "" : args.positional[0];
    loadRoomIntoStateImpl(st, initial);
    if (st.currentRoomId.empty() && !st.rooms.empty()) {
        loadRoomIntoStateImpl(st, st.rooms.front().value("room_id", ""));
    }


    // --static thread <room> / --static threads: the right panel becomes
    // the thread list instead of a room being opened.
    if (initial == "thread" && args.positional.size() >= 2) {
        loadRoomIntoStateImpl(st, args.positional[1]);
        st.rightPanel = 1;
        st.threadRoomId = st.currentRoomId;
    } else if (initial == "threads") {
        st.rightPanel = 3;
        if (args.positional.size() >= 2) loadRoomIntoStateImpl(st, args.positional[1]);
    }
    // Non-interactive flags (also usable in the REPL): --ids, --time-full,
    // --right members|threads, --limit N.
    if (args.options.count("ids")) st.showIds = true;
    if (args.options.count("time-full") || args.options.count("sec")) st.showSeconds = true;
    if (args.options.count("time-side")) {
        st.timeRight = args.options.at("time-side") == "right";
    }
    if (args.options.count("msg-line")) {
        st.msgNewline = args.options.at("msg-line") == "newline";
    }
    if (args.options.count("no-emoji")) st.showEmoji = false;
    if (args.options.count("limit")) {
        try { st.limit = std::stoi(args.options.at("limit")); } catch (...) {}
        loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
    }
    if (args.options.count("right")) {
        std::string r = args.options.at("right");
        if (r == "threads") {
            st.rightPanel = 3;
        } else if (r == "agent") {
            st.rightPanel = 4;
        } else if (r == "list") {
            st.rightPanel = 1;
            st.threadRoomId = st.currentRoomId;
        } else if (r == "thread") {
            std::string sel;
            if (args.options.count("thread-root")) sel = args.options.at("thread-root");
            else if (args.options.count("thread")) sel = args.options.at("thread");
            st.rightPanel = 2;
            st.threadRoomId = st.currentRoomId;
            st.threadRootId = sel;
            std::string root = resolveThreadRoot(st.db, st.currentRoomId, sel);
            if (root.empty()) {
                st.rightPanel = 0;
                st.statusNote = "thread not found: " + sel;
            } else {
                st.threadRootId = root;
                auto events = st.db->getEvents(st.currentRoomId, 300);
                st.threadReplies.clear();
                for (const auto& ev : events) {
                    if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
                }
            }
        }
    }
    // --thread <N|id>: the thread panel of the open room, no --right needed.
    if (args.options.count("thread") && st.rightPanel != 2) {
        std::string sel = args.options.at("thread");
        std::string root = resolveThreadRoot(st.db, st.currentRoomId, sel);
        if (root.empty()) {
            st.statusNote = "thread not found: " + sel;
        } else {
            st.rightPanel = 2;
            st.threadRoomId = st.currentRoomId;
            st.threadRootId = root;
            auto events = st.db->getEvents(st.currentRoomId, 300);
            st.threadReplies.clear();
            for (const auto& ev : events) {
                if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
            }
        }
    }

    if (args.options.count("static") || args.options.count("once") ||
        args.options.count("print")) {
        st.staticFrame = true;
    }
    // Settings for the one-shot frame: --rows N (frame height) and
    // --scroll N (viewport offset) — the room list scrolls within it.
    if (args.options.count("mobile")) st.mobile = true;
    if (args.options.count("space")) {
        st.activeSpace = resolveSpace(st.rooms, args.options.at("space"));
    }
    // Temporary one-shot layout flags (not persisted, unlike the REPL
    // commands panel/members/rows which save to the settings table):
    // --panel-left/--panel-right <off|on|width>, --panel-auto on|off,
    // --members horizontal|list|auto.
    auto parsePanel = [](const std::string& v) -> int {
        if (v == "off") return 0;
        if (v == "on" || v.empty()) return -1;
        try { return std::stoi(v); } catch (...) { return -1; }
    };
    if (args.options.count("panel-left")) {
        st.leftPanelW = parsePanel(args.options.at("panel-left"));
        st.autoPanels = false;
    }
    if (args.options.count("panel-right")) {
        st.rightPanelW = parsePanel(args.options.at("panel-right"));
        st.autoPanels = false;
    }
    if (args.options.count("panel-auto")) {
        st.autoPanels = args.options.at("panel-auto") != "off";
    }
    if (args.options.count("members")) {
        std::string m = args.options.at("members");
        if (m == "horizontal") st.membersMode = 1;
        else if (m == "list" || m == "vertical") st.membersMode = 2;
        else st.membersMode = 0;
    }
    // Element Classic: with a room on the command line, open it in the
    // Chat tab right away; without one, land on the Rooms tab.
    if (st.mobile && !initial.empty()) st.mobileTab = 1;
    if (args.options.count("rows")) {
        try { st.limitRows = std::stoi(args.options.at("rows")); } catch (...) {}
    }
    if (args.options.count("scroll")) {
        try { st.scroll = std::stoi(args.options.at("scroll")); } catch (...) {}
    }
    // Relative scroll steps on top of the base: --scroll-line (one row),
    // --scroll-lines N (a custom amount) and --scroll-page [N] (whole
    // viewport pages — the page size is the frame height, resolved at
    // draw time). They stack with --scroll (and with each other).
    if (args.options.count("scroll-line")) st.scrollStep += 1;
    if (args.options.count("scroll-lines")) {
        try { st.scrollStep += std::stoi(args.options.at("scroll-lines")); } catch (...) {}
    }
    if (args.options.count("scroll-page")) {
        const std::string& v = args.options.at("scroll-page");
        try { st.scrollPage += v.empty() || v == "true" ? 1 : std::stoi(v); } catch (...) {}
    }
    // Per-panel scrolling: --scroll-center (or --scroll) moves the chat,
    // --scroll-left only the rooms list, --scroll-right only the right
    // panel. Negatives count from the bottom of each panel.
    if (args.options.count("scroll-center")) {
        try { st.scroll = std::stoi(args.options.at("scroll-center")); } catch (...) {}
    }
    if (args.options.count("scroll-right")) {
        try { st.rightScroll = std::stoi(args.options.at("scroll-right")); } catch (...) {}
    }
    // --jump <YYYY-MM-DD>: position the viewport at that day (static).
    if (args.options.count("jump")) {
        int64_t dayMs = parseDayMs(args.options.at("jump"));
        if (dayMs > 0) {
            int64_t best = 0;
            std::string bestId;
            for (const auto& ev : st.messages) {
                if (ev.origin_server_ts >= dayMs) {
                    best = ev.origin_server_ts;
                    bestId = ev.event_id;
                    break;
                }
            }
            if (!bestId.empty()) {
                int row = centerRowIndexOf(st, bestId);
                if (row >= 0) {
                    st.scroll = std::max(0, row - 12);
                    st.focusEvent = bestId;
                    st.statusNote = "jumped to " + args.options.at("jump");
                }
            }
        }
    }
    if (args.options.count("scroll-left")) {
        try { st.leftScroll = std::stoi(args.options.at("scroll-left")); } catch (...) {}
    }
    if (args.options.count("scroll-threads")) {
        try { st.threadsScroll = std::stoi(args.options.at("scroll-threads")); } catch (...) {}
    }
    std::cout << drawFrameImpl(st) << std::flush;

    // --agent <prompt>: run the local coding agent after the frame.
    if (args.options.count("agent")) {
        agenttools::Config cfg;
        cfg.provider = dbi.getSetting("agent_provider", "openai");
        cfg.endpoint = dbi.getSetting("agent_endpoint", "");
        cfg.model = dbi.getSetting("agent_model", "");
        cfg.key = dbi.getSetting("agent_key", "");
        cfg.trust = dbi.getSetting("agent_trust", "ask");
        {
            auto loadCsv = [&](const std::string& key,
                               std::vector<std::string>& out) {
                std::string v = dbi.getSetting(key, "");
                std::string cur;
                for (char ch : v) {
                    if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
                    else cur += ch;
                }
                if (!cur.empty()) out.push_back(cur);
            };
            loadCsv("agent_allow", cfg.allowPrefixes);
            loadCsv("agent_deny", cfg.denyPrefixes);
        }
        if (args.options.count("agent-provider")) cfg.provider = args.options.at("agent-provider");
        if (args.options.count("agent-endpoint")) cfg.endpoint = args.options.at("agent-endpoint");
        if (args.options.count("agent-model")) cfg.model = args.options.at("agent-model");
        if (args.options.count("agent-key")) cfg.key = args.options.at("agent-key");
        if (args.options.count("agent-trust")) cfg.trust = args.options.at("agent-trust");
        if (args.options.count("agent-proxy")) cfg.proxy = args.options.at("agent-proxy");
        if (cfg.key.empty()) {
            const char* env = cfg.provider == "anthropic"
                                  ? std::getenv("ANTHROPIC_API_KEY")
                                  : std::getenv("OPENAI_API_KEY");
            if (env && *env) cfg.key = env;
        }
        if (cfg.model.empty()) {
            cfg.model = cfg.provider == "anthropic"
                            ? "claude-3-5-haiku-20241022" : "gpt-4o-mini";
        }
        char cwdbuf[4096];
        if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;
        std::vector<agenttools::Message> history;
        agenttools::Result res = agenttools::run(cfg, args.options.at("agent"),
            history,
            [&](const std::string& cmd) -> int {
                std::cout << "run: " << cmd << " [y/N/a/A] " << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                if (ans == "y" || ans == "Y") return 1;
                if (ans == "a") return 2;
                if (ans == "A") return 3;
                return 0;
            },
            [&](const std::string& questionsJson) -> std::string {
                nlohmann::json qs;
                try { qs = nlohmann::json::parse(questionsJson); }
                catch (...) { return "error: bad questions JSON"; }
                std::string out;
                for (const auto& q : qs.value("questions", nlohmann::json::array())) {
                    std::cout << "  Q: " << q.value("question", "?") << std::endl;
                    auto opts = q.value("options", nlohmann::json::array());
                    for (size_t i = 0; i < opts.size(); ++i) {
                        std::cout << "    " << i + 1 << ") "
                                  << opts[i].value("label", "") << std::endl;
                    }
                    std::cout << "  answer> " << std::flush;
                    std::string ans;
                    std::getline(std::cin, ans);
                    out += "Q: " + q.value("question", "?") + "\nA: " + ans + "\n";
                }
                return out.empty() ? "(no questions answered)" : out;
            },
            [](const std::string& l) { std::cout << l << std::endl; },
            [](const std::string& t) { std::cout << t << std::flush; });
        if (!res.ok) std::cout << "[agent error] " << res.error << std::endl;
        else if (!res.streamed) std::cout << res.text << std::endl;
        mkdir(".agent-sessions", 0755);
        agenttools::saveSession(".agent-sessions/last.json", history);
    }

    // Pure CLI / non-interactive mode: draw the frame once and exit
    // (pipe-friendly: progressive-cli ui --static [room] | less).
    if (args.options.count("static") || args.options.count("once") ||
        args.options.count("print")) {
        // Static mode: media previews only with --media (opt-in).
        if (args.options.count("media")) {
        for (const auto& ev : st.messages) {
            if (ev.type != "m.room.message" && ev.type != "m.sticker") continue;
            if (!ev.content.is_object()) continue;
            bool isImage = false;
            auto mt = ev.content.find("msgtype");
            if (mt != ev.content.end() && mt->is_string() &&
                mt->get<std::string>() == "m.image") isImage = true;
            if (ev.type == "m.sticker") isImage = true;
            if (!isImage) continue;
            std::string mxc;
            auto urlIt = ev.content.find("url");
            if (urlIt != ev.content.end() && urlIt->is_string()) mxc = urlIt->get<std::string>();
            if (mxc.empty()) continue;
            std::string fname = "media_" + ev.event_id.substr(0, 12) + ".bin";
            if (mxc.find("demo.local") != std::string::npos) {
                std::cout << "── " << eventBody(ev) << " (demo preview) ──" << std::endl;
                std::string tmpImg = "/tmp/matrixcli_preview.png";
                std::string gen = "magick -size 320x160 gradient:orange-red "
                    "-fill white -pointsize 20 -gravity center "
                    "-annotate 0 'demo image' '" + tmpImg
                    + "' 2>/dev/null || convert -size 320x160 "
                    "gradient:orange-red -fill white -pointsize 20 "
                    "-gravity center -annotate 0 'demo image' '" + tmpImg
                    + "' 2>/dev/null";
                std::system(gen.c_str());
                std::string cmd;
                if (std::system("which chafa >/dev/null 2>&1") == 0) {
                    cmd = "chafa --format symbols --size 60x20 '" + tmpImg + "' 2>/dev/null";
                } else if (std::system("which img2txt >/dev/null 2>&1") == 0) {
                    cmd = "img2txt -W 60 -H 20 '" + tmpImg + "' 2>/dev/null";
                }
                if (!cmd.empty()) std::system(cmd.c_str());
                std::remove(tmpImg.c_str());
            }
        }
        }
        return 0;
    }

    std::string line;
    std::vector<std::string> history;
    for (;;) {
        if (!matrixcli::g_interrupted.load()) break;  // Ctrl+C
        if (!readLineWithHistory(history, "ui> ", line)) break;
        if (!matrixcli::g_interrupted.load()) break;
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        cli::Args a;
        {
            std::istringstream iss(line);
            std::vector<std::string> words;
            std::string w;
            while (iss >> w) words.push_back(w);
            a.command = words[0];
            static const std::unordered_set<std::string> noValueFlags = {
                "static", "once", "print", "json", "confirm", "debug", "ts",
                "ids", "expand", "verbose", "all", "interactive", "help",
                "cli", "ui", "ascii", "populate", "no-replies", "no-filter",
            };
            for (size_t i = 1; i < words.size(); ++i) {
                if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
                    std::string key = words[i].substr(2);
                    auto eq = key.find('=');
                    if (eq != std::string::npos) {
                        a.options[key.substr(0, eq)] = key.substr(eq + 1);
                    } else if (i + 1 < words.size() && !noValueFlags.count(key) &&
                               !(words[i + 1].size() >= 2 && words[i + 1][0] == '-')) {
                        a.options[key] = words[++i];
                    } else {
                        a.options[key] = "true";
                    }
                } else {
                    a.positional.push_back(words[i]);
                }
            }
        }
        int ra = asciiReplDispatchA(st, dbi, a);
        if (ra == 2) break;
        if (ra == 1) continue;
        int rb = asciiReplDispatchB(st, dbi, a);
        if (rb == 2) break;
        if (rb == 1) continue;
        int rc = asciiAgentReplDispatch(st, dbi, a);
        if (rc == 2) break;
        if (rc == 1) continue;
        int rg = asciiReplDispatchG(st, dbi, a);
        if (rg == 2) break;
        if (rg == 1) continue;
        int re = asciiReplDispatchE(st, dbi, a);
        if (re == 2) break;
        if (re == 1) continue;
        std::cout << "Unknown command '" << a.command << "' — type 'help'.\n";
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

} // namespace matrixcli
