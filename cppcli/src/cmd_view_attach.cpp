#include <iostream>
#include <csignal>
#include <signal.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
#include <map>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "config.hpp"
#include "media_send.hpp"
#include "main_commands.hpp"
#include "commands.hpp"
#include "core/http_client.hpp"
#include "core/crypto/media_crypto.hpp"
#include <simdjson.h>
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "core/crash_handler.hpp"
#include "server/server.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/tdlib/tdlib_bridge.hpp"
#include "../lib/irc/irc_client.hpp"
#include "../lib/lemmy/lemmy_client.hpp"
#include "../lib/deltachat/dc_bridge.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/client_utils.hpp"
#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#endif


int cmdView(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli view <room> [limit] [--thread event_id] [--before eid] [--from eid]\n"
                     "       [--senders @u:h,@u2:h] [--hide @u:h] [--replies N|off] [--no-replies]\n"
                     "       [--no-filter] [--json] [--expand] [--verbose] [--ts]"
                  << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    int limit = 20;
    if (args.positional.size() >= 2 && !args.positional[1].starts_with("--")) {
        std::string lv = args.positional[1];
        limit = (lv == "all" || lv == "0") ? -1 : std::stoi(lv);
    }
    auto lm = args.options.find("limit");
    if (lm != args.options.end()) {
        std::string lv = lm->second;
        limit = (lv == "all" || lv == "0") ? -1 : std::stoi(lv);
    }

    std::string thread_root;
    auto tr_it = args.options.find("thread");
    if (tr_it != args.options.end()) thread_root = tr_it->second;

    std::string before;
    auto bf_it = args.options.find("before");
    if (bf_it != args.options.end()) before = bf_it->second;

    std::string from;
    auto fm_it = args.options.find("from");
    if (fm_it != args.options.end()) from = fm_it->second;

    // Temporary sender filters (this invocation only)
    auto splitMxids = [](const std::string& csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    std::vector<std::string> tmp_senders, tmp_hide;
    auto sn_it = args.options.find("senders");
    if (sn_it != args.options.end()) tmp_senders = splitMxids(sn_it->second);
    auto hd_it = args.options.find("hide");
    if (hd_it != args.options.end()) tmp_hide = splitMxids(hd_it->second);
    bool no_filter = args.options.count("no-filter");

    // Reply chains (element-web style): show the replied-to message(s), and one
    // level further if that message is itself a reply. Depth defaults to 3.
    bool show_replies = true;
    int reply_depth = 3;
    if (args.options.count("no-replies")) show_replies = false;
    auto rp_it = args.options.find("replies");
    if (rp_it != args.options.end()) {
        std::string rv = rp_it->second;
        if (rv == "0" || rv == "off" || rv == "no") show_replies = false;
        else { try { reply_depth = std::stoi(rv); if (reply_depth <= 0) show_replies = false; } catch (...) {} }
    }

    bool verbose = args.options.count("verbose") || args.options.count("ids");
    bool show_ts = args.options.count("ts") || args.options.count("time");
    bool debug = args.options.count("debug") || args.options.count("raw");
    bool json_out = args.options.count("json");
    bool expand = args.options.count("expand") || args.options.count("full");

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database" << std::endl; return 1; }

    std::string room_id;
    std::string room_name;
    bool found = false;
    auto rooms = dbi.listRooms();
    for (auto& r : rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == query || name == query || name.find(query) == 0) {
            room_id = id;
            room_name = name;
            found = true;
            break;
        }
    }
    if (room_id.empty()) { room_id = query; room_name = query; }

    auto events = dbi.getEvents(room_id, limit > 0 ? limit : 999999, before, from);

    // Effective filters: temporary flags (--senders/--hide) take precedence over
    // permanent config filters (per-room, then global). --no-filter disables all.
    Config::instance().load("config.json");
    std::vector<std::string> senders_filter = tmp_senders;
    std::vector<std::string> hide_filter = tmp_hide;
    if (!no_filter) {
        nlohmann::json flt = Config::instance().filters();
        auto applyJsonList = [](const nlohmann::json& j, const std::string& key, std::vector<std::string>& out) {
            if (j.is_object() && j.contains(key) && j[key].is_array())
                for (auto& v : j[key]) if (v.is_string()) out.push_back(v.get<std::string>());
        };
        nlohmann::json room_flt;
        if (flt.contains("rooms") && flt["rooms"].is_object() && flt["rooms"].contains(room_id))
            room_flt = flt["rooms"][room_id];
        if (senders_filter.empty()) applyJsonList(room_flt, "senders", senders_filter);
        if (senders_filter.empty()) applyJsonList(flt, "senders", senders_filter);
        applyJsonList(room_flt, "hide", hide_filter);
        applyJsonList(flt, "hide", hide_filter);
    }
    if (!senders_filter.empty() || !hide_filter.empty()) {
        events.erase(std::remove_if(events.begin(), events.end(), [&](const matrix::Event& e) {
            if (!senders_filter.empty()) {
                bool ok = false;
                for (auto& s : senders_filter) if (e.sender == s) { ok = true; break; }
                if (!ok) return true;
            }
            for (auto& h : hide_filter) if (e.sender == h) return true;
            return false;
        }), events.end());
    }

    // JSON output mode (pipe-friendly: pure JSON on stdout, chronological order)
    if (json_out) {
        nlohmann::json j;
        j["room_id"] = room_id;
        j["known"] = found;
        j["messages"] = nlohmann::json::array();
        if (from.empty()) std::reverse(events.begin(), events.end());
        for (auto& ev : events) {
            nlohmann::json m;
            m["event_id"] = ev.event_id;
            m["sender"] = ev.sender;
            m["type"] = ev.type;
            m["msgtype"] = ev.content.value("msgtype", "m.text");
            m["body"] = ev.content.value("body", "");
            m["ts"] = ev.origin_server_ts;
            m["reply_to"] = nlohmann::json::array();
            if (ev.content.contains("m.relates_to") && ev.content["m.relates_to"].contains("m.in_reply_to")) {
                std::string cur = ev.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
                for (int lvl = 0; !cur.empty() && lvl < reply_depth; ++lvl) {
                    matrix::Event anc;
                    if (!dbi.getEventById(cur, anc)) break;
                    nlohmann::json r;
                    r["event_id"] = anc.event_id;
                    r["sender"] = anc.sender;
                    r["body"] = anc.content.value("body", "");
                    m["reply_to"].push_back(r);
                    cur.clear();
                    if (anc.content.contains("m.relates_to") && anc.content["m.relates_to"].contains("m.in_reply_to"))
                        cur = anc.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
                }
            }
            j["messages"].push_back(m);
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    if (events.empty()) {
        if (!before.empty()) std::cout << "(no older messages)" << std::endl;
        else std::cout << "(no messages in cache)" << std::endl;
        return 0;
    }

    if (from.empty()) std::reverse(events.begin(), events.end());

    // Human-mode header
    if (!thread_root.empty()) std::cout << "=== " << room_name << " / thread " << thread_root << " ===" << std::endl;
    else std::cout << "=== " << room_name << " (" << room_id << ") ===" << std::endl;

    // Show pagination hint
    bool has_newer, has_older;
    if (!from.empty()) {
        has_older = true;                              // history before the anchor exists
        has_newer = (int)events.size() >= limit;       // window full -> more newer
    } else if (!before.empty()) {
        has_newer = true;
        has_older = (int)events.size() >= limit;
    } else {
        has_newer = false;
        has_older = (int)events.size() >= limit;
    }

    // Message grouping
    std::string prev_sender;

    if (has_newer || has_older) {
        std::cout << "── ";
        if (has_newer) std::cout << "view --from " << events.back().event_id << " (newer)  ";
        if (has_older) std::cout << "view --before " << events.front().event_id << " (older)";
        std::cout << " ──" << std::endl;
    }

    for (auto& ev : events) {
        // Filter to thread if requested
        bool in_thread = false;
        if (!thread_root.empty()) {
            if (ev.content.contains("m.relates_to") &&
                ev.content["m.relates_to"].value("rel_type", "") == "m.thread" &&
                ev.content["m.relates_to"].value("event_id", "") == thread_root) {
                in_thread = true;
            } else if (ev.event_id != thread_root) {
                continue;
            }
        }

        // Ephemeral events are not timeline rows: receipts show in the
        // notifications corner instead.
        if (ev.type == "m.receipt") continue;
        std::string body = ev.content.value("body", "(no body)");
        // element-web style: strip the fallback "> quote" block from reply bodies
        // (the quote is rendered as reply context instead).
        auto stripFallbackQuote = [](std::string& s) {
            if (s.compare(0, 2, "> ") != 0) return;
            size_t start = 0;
            while (start < s.size()) {
                if (s.compare(start, 2, "> ") == 0) {
                    auto nl = s.find('\n', start);
                    if (nl == std::string::npos) { start = s.size(); break; }
                    start = nl + 1;
                } else break;
            }
            while (start < s.size() && (s[start] == '\n' || s[start] == ' ')) start++;
            s = start >= s.size() ? "" : s.substr(start);
        };
        if (show_replies && ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].contains("m.in_reply_to")) {
            stripFallbackQuote(body);
        }
        if (!expand && body.size() > 120) body = body.substr(0, 120) + "...";

        // Basic markdown → ANSI
        std::string md_body;
        for (size_t i = 0; i < body.size(); i++) {
            if (body[i] == '*' && i+1 < body.size() && body[i+1] == '*') {
                i += 2; md_body += ANSI_BOLD;
                while (i < body.size() && !(body[i] == '*' && i+1 < body.size() && body[i+1] == '*'))
                    md_body += body[i++];
                md_body += ANSI_RESET;
                if (i+1 < body.size()) i++;
                continue;
            }
            if (body[i] == '*' && i > 0 && body[i-1] == ' ') {
                i++; md_body += ANSI_ITALIC;
                while (i < body.size() && body[i] != '*') md_body += body[i++];
                md_body += ANSI_RESET;
                continue;
            }
            if (body[i] == '`') {
                i++; md_body += ANSI_DIM;
                while (i < body.size() && body[i] != '`') md_body += body[i++];
                md_body += ANSI_RESET;
                continue;
            }
            md_body += body[i];
        }
        std::string sender = ev.sender;
        std::string sender_name = util::userIdToName(sender);

        std::string ts_str;
        if (show_ts) ts_str = " " + relativeTime(ev.origin_server_ts);

        // Member events (join/leave/invite)
        std::string member_line;
        if (ev.type == "m.room.member" && ev.content.contains("membership")) {
            std::string membership = ev.content["membership"].get<std::string>();
            std::string displayname = ev.content.value("displayname", ev.state_key.empty() ? ev.sender : ev.state_key);
            if (displayname.starts_with("@")) displayname = displayname.substr(1);
            if (membership == "join") member_line = "→ " + displayname + " joined";
            else if (membership == "leave") member_line = "← " + displayname + " left";
            else if (membership == "invite") member_line = "✉ " + displayname + " invited";
            else if (membership == "ban") member_line = "⛔ " + displayname + " banned";
            else if (membership == "knock") member_line = "✊ " + displayname + " knocked";
        }

        if (!member_line.empty()) {
            std::cout << "  " ANSI_DIM "-- " << member_line << " --" ANSI_RESET << ts_str << std::endl;
            if (debug) std::cout << ANSI_GRAY "       id:" << ev.event_id << " state_key:" << ev.state_key << ANSI_RESET << std::endl;
            continue;
        }

        // Day separator
        static int64_t last_day = 0;
        time_t msg_t = ev.origin_server_ts / 1000;
        struct tm msg_tm;
        localtime_r(&msg_t, &msg_tm);
        msg_tm.tm_hour = 0; msg_tm.tm_min = 0; msg_tm.tm_sec = 0;
        int64_t msg_day = mktime(&msg_tm);
        if (msg_day != last_day && msg_day > 0) {
            last_day = msg_day;
            std::cout << std::endl << "  " ANSI_BOLD ANSI_CYAN << daySeparator(ev.origin_server_ts) << ANSI_RESET << std::endl << std::endl;
        }
        std::string prefix;
        if (ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].value("rel_type", "") == "m.thread") {
            prefix = "↳ ";
        }

        // Count thread replies
        int reply_count = 0;
        for (auto& other : events) {
            if (other.content.contains("m.relates_to") &&
                other.content["m.relates_to"].value("rel_type", "") == "m.thread" &&
                other.content["m.relates_to"].value("event_id", "") == ev.event_id) {
                reply_count++;
            }
        }

        // Reply context — element-web style multilevel chain: show the
        // replied-to message(s), one level deeper if those are replies too.
        if (show_replies && ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].contains("m.in_reply_to")) {
            std::string cur = ev.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
            for (int lvl = 0; !cur.empty() && lvl < reply_depth; ++lvl) {
                matrix::Event anc;
                if (!dbi.getEventById(cur, anc)) break;
                if (anc.type == "m.room.message" || anc.type == "m.text" || anc.type == "m.emote") {
                    std::string abody = anc.content.value("body", "");
                    stripFallbackQuote(abody);   // ancestors may be replies themselves
                    if (!expand && abody.size() > 100) abody = abody.substr(0, 100) + "...";
                    std::cout << "    " << std::string(lvl * 2, ' ') << ANSI_DIM "↱ "
                              << util::userIdToName(anc.sender) << ": " << abody << ANSI_RESET << std::endl;
                }
                cur.clear();
                if (anc.content.contains("m.relates_to") && anc.content["m.relates_to"].contains("m.in_reply_to"))
                    cur = anc.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
            }
        }

        // Link detection
        std::string link = extractLink(body);
        if (!link.empty() && !show_ts) {
            if (body.size() > 80) body = body.substr(0, 77) + "...";
        }

        std::string reply_str;
        if (reply_count > 0) reply_str = " [" + std::to_string(reply_count) + " replies]";

        // Message grouping: collapse sender if same as previous
        if (ev.sender == prev_sender && !prev_sender.empty()) {
            std::string indent(sender_name.size() + 3, ' ');
            std::cout << indent << prefix << md_body << reply_str;
        } else {
            prev_sender = ev.sender;
            std::cout << "  " << prefix << ansiUser(ev.sender, "[" + sender_name + "]") << ts_str << " " << md_body << reply_str;
        }

        // Show replied-to body if available
        if (ev.content.contains("m.relates_to")) {
            auto& rel = ev.content["m.relates_to"];
            if (rel.value("rel_type", "") == "m.in_reply_to" && ev.content.contains("m.new_content")) {
                std::string old_body = ev.content["m.new_content"].value("body", "");
                if (!old_body.empty())
                    std::cout << "\n" ANSI_GRAY "       ↪ \"" << old_body.substr(0, 60) << (old_body.size() > 60 ? "..." : "") << "\"" ANSI_RESET;
            }
        }
        if (verbose) {
            std::cout << "\n" ANSI_GRAY "       id:" << ev.event_id;
            if (!ev.redacts.empty()) std::cout << " redacts:" << ev.redacts;
            if (!ev.state_key.empty()) std::cout << " state_key:" << ev.state_key;
            std::cout << ANSI_RESET;
        }
        if (debug) {
            std::cout << "\n" ANSI_DIM "       raw:" << ev.content.dump() << ANSI_RESET;
        }
        std::cout << std::endl;
    }
    return 0;
}namespace matrixcli {
int cmdAttachFile(const cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: progressive-cli attach <room> <file> [--caption text] [--thread event_id] [--chunks N]" << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    std::string path = args.positional[1];
    std::string caption = args.options.count("caption") ? args.options.at("caption") : "";
    std::string thread_root = args.options.count("thread") ? args.options.at("thread") : "";
    int chunks = 0;
    if (args.options.count("chunks")) {
        try { chunks = std::stoi(args.options.at("chunks")); } catch (...) {}
    }

    // Read the file.
    std::ifstream fin(path, std::ios::binary);
    if (!fin) { std::cerr << "Cannot open file: " << path << std::endl; return 1; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) { std::cerr << "Empty file: " << path << std::endl; return 1; }

    // Extension -> msgtype + content type.
    std::string fn = path;
    auto slash = fn.find_last_of('/');
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    auto dot = fn.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : fn.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(::tolower(c));
    std::string mt, ct;
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "webp" || ext == "bmp" || ext == "svg") {
        mt = "m.image";
        ct = ext == "jpg" ? "image/jpeg" : "image/" + ext;
    } else if (ext == "mp4" || ext == "webm" || ext == "mov" || ext == "mkv") {
        mt = "m.video";
        ct = "video/" + ext;
    } else if (ext == "mp3" || ext == "ogg" || ext == "wav" || ext == "flac" ||
               ext == "opus" || ext == "m4a") {
        mt = "m.audio";
        ct = ext == "m4a" ? "audio/mp4" : "audio/" + ext;
    } else {
        mt = "m.file";
        ct = "application/octet-stream";
    }
    std::string bodyName = caption.empty() ? fn : caption;

    // The send preset: original (as-is), compact (the minimal event),
    // full (plus the info block: size, and the image dimensions).
    std::string preset = "original";
    {
        db::Database dbi;
        if (dbi.open("matrixcli.db")) preset = dbi.getSetting("send_preset", "original");
        if (preset != "compact" && preset != "full") preset = "original";
    }
    int imgW = 0, imgH = 0;
    bool hasImgDim = mt == "m.image" && matrixcli::media::imageDimensions(bytes, ext, imgW, imgH);

    if (!pcore::init() || !pcore::loadSavedSession()) {
        std::cerr << "Not logged in. Run 'progressive-cli login' first." << std::endl;
        return 1;
    }
    auto& core = pcore::core();
    std::string bootNote = pcore::bootstrap();
    if (!bootNote.empty()) { std::cerr << "Warning: " << bootNote << std::endl; return 1; }

    std::string room_id = query;
    {
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == query || name == query || name.find(query) == 0 ||
                    id.find(query) != std::string::npos) {
                    room_id = id;
                    break;
                }
            }
        }
    }
    auto client = core.client;
    std::string hs = client->account().homeserverUrl;
    std::string token = client->account().accessToken;

    bool encrypted = client->isRoomEncrypted(room_id);

    // Live upload progress on stderr (the callback runs on the curl thread).
    int lastPct = -1;
    bool progressDone = false;
    progressive::desktop::setHttpProgressCallback(
        [&lastPct, &progressDone](int64_t ul, int64_t ulTotal,
                                  int64_t, int64_t) {
            if (ulTotal > 0 && !progressDone) {
                int pct = static_cast<int>(ul * 100 / ulTotal);
                if (pct != lastPct) {
                    lastPct = pct;
                    std::fprintf(stderr, "\rupload: %d%% (%lld / %lld bytes)%s",
                                 pct, static_cast<long long>(ul),
                                 static_cast<long long>(ulTotal),
                                 pct >= 100 ? "\n" : "   ");
                    std::fflush(stderr);
                }
                if (pct >= 100) progressDone = true;
            }
        });
    auto finishProgress = [&progressDone]() {
        if (!progressDone) std::fprintf(stderr, "\n");
        progressive::desktop::setHttpProgressCallback({});
        progressDone = true;
    };

    auto doUpload = [&](const std::vector<uint8_t>& d) -> std::string {
        if (chunks > 1) {
            matrix::Client mclient;
            {
                db::Database dbi;
                if (dbi.open("matrixcli.db")) {
                    auto acc = dbi.loadAccount();
                    if (acc.is_logged_in()) {
                        mclient.setHomeserverURL(acc.homeserver_url);
                        mclient.setAccessToken(acc.access_token);
                        mclient.setUserId(acc.user_id);
                    }
                }
            }
            try {
                return mclient.uploadMediaChunked(d, fn, ct, chunks);
            } catch (const std::exception& e) {
                std::cerr << "Chunked upload failed: " << e.what() << std::endl;
                return "";
            }
        }
        auto r = client->uploadMedia(d, fn, ct);
        if (!r.ok) {
            std::cerr << "Upload failed: " << r.error.message << std::endl;
            return "";
        }
        return r.data;
    };
    std::string mxc = doUpload(bytes);
    finishProgress();
    if (mxc.empty()) return 1;

    if (!encrypted) {
        std::string content = matrixcli::media::plainContent(
            mt, bodyName, fn, mxc, ct, bytes.size(), imgW, imgH, hasImgDim,
            preset, thread_root);
        auto r = client->sendMessageEvent(room_id, "m.room.message", content);
        if (!r.ok) { std::cerr << "Send failed: " << r.error.message << std::endl; return 1; }
        std::cout << "Sent " << mt << " to " << room_id
                  << (thread_root.empty() ? "" : " (thread " + thread_root + ")")
                  << (preset == "original" ? "" : " (preset " + preset + ")")
                  << " (mxc " << mxc << ")" << std::endl;
        return 0;
    }

    // Encrypted room: AES-CTR the file + the Element "file" block.
    std::string encKey, encIv;
    if (!progressive::desktop::generateMediaKeyIv(encKey, encIv)) {
        std::cerr << "Media key generation failed" << std::endl;
        return 1;
    }
    std::vector<uint8_t> encBytes = progressive::desktop::aesCtrCrypt(bytes, encKey, encIv);
    if (encBytes.empty()) { std::cerr << "Media encryption failed" << std::endl; return 1; }
    std::string encSha = progressive::desktop::sha256Base64(bytes);
    int encLastPct = -1;
    bool encDone = false;
    progressive::desktop::setHttpProgressCallback(
        [&encLastPct, &encDone](int64_t ul, int64_t ulTotal, int64_t, int64_t) {
            if (ulTotal > 0 && !encDone) {
                int pct = static_cast<int>(ul * 100 / ulTotal);
                if (pct != encLastPct) {
                    encLastPct = pct;
                    std::fprintf(stderr, "\rupload: %d%% (%lld / %lld bytes)%s",
                                 pct, static_cast<long long>(ul),
                                 static_cast<long long>(ulTotal),
                                 pct >= 100 ? "\n" : "   ");
                    std::fflush(stderr);
                }
                if (pct >= 100) encDone = true;
            }
        });
    mxc = doUpload(encBytes);
    if (!encDone) std::fprintf(stderr, "\n");
    progressive::desktop::setHttpProgressCallback({});
    if (mxc.empty()) {
        std::cerr << "Upload failed." << std::endl;
        return 1;
    }
    std::string fbodyStr = matrixcli::media::encryptedContent(
        mt, bodyName, fn, mxc, encKey, encIv, encSha, ct, bytes.size(),
        imgW, imgH, hasImgDim, preset, thread_root);
    std::string inner = "{\"type\":\"m.room.message\",\"content\":" + fbodyStr +
                        ",\"room_id\":\"" + room_id + "\"}";
    auto* dec = core.sync->decryptor();
    std::string sessId = dec->getOrCreateOutboundSession(room_id);
    if (sessId.empty()) {
        std::cerr << "Could not create the outbound megolm session" << std::endl;
        return 1;
    }
    std::string enc = dec->encryptMessage(room_id, client->account().deviceId, inner);
    if (enc.empty()) { std::cerr << "Encryption failed" << std::endl; return 1; }
    if (!dec->roomKeyShared(room_id)) {
        auto members = client->getRoomMembers(room_id);
        std::vector<std::string> memberIds;
        if (members.ok) {
            try {
                auto j = nlohmann::json::parse(members.data);
                for (auto& [uid, info] : j["chunk"].items()) {
                    (void)info;
                    memberIds.push_back(uid);
                }
            } catch (...) {}
        }
        bool shared = dec->shareRoomKey(room_id, memberIds,
                                        client->account().userId,
                                        client->account().deviceId, hs, token);
        if (!shared) {
            std::cerr << "Warning: room key share failed — the receiver may not "
                         "decrypt this file yet." << std::endl;
        }
        dec->markRoomKeyShared(room_id);
    }
    auto r = client->sendEncryptedEvent(room_id, enc, "txn" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    if (!r.ok) { std::cerr << "Send failed: " << r.error.message << std::endl; return 1; }
    std::cout << "Sent encrypted " << mt << " to " << room_id << std::endl;
    return 0;
}

} // namespace matrixcli
