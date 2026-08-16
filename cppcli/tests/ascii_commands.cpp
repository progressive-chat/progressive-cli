// src/ascii_commands.cpp — the REPL command handlers (split out of
// ascii_ui.cpp so the compilation stays incremental-friendly).
#include "ascii_state.hpp"

#include "../lib/database/db.hpp"
#include "pcore.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace matrixcli {

bool asciiCommandDispatch(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "dump" || a.command == "export") {
            if (a.positional.empty()) {
                std::cout << "Usage: dump <room> [room2 ...] [--format json|txt|html]"
                             " [--out dir] [--media] [--server]"
                          << std::endl;
                std::cout << "  --media          download media files (default: no media)\n"
                             "  --limit N        export only the last N events\n"
                             "  --order asc|desc chronological (default asc, oldest first)\n"
                             "  --types list     segment by type: messages|media|system"
                             " (comma list, default all)\n"
                             "  --media-max MB   skip media files larger than MB megabytes\n"
                             "  --archive zip|tar.gz  pack the export into an archive"
                             " (zip needs the 'zip' tool)\n"
                             "  --server        dump the FULL history from the server (paginated),"
                             " not just the cache"
                          << std::endl;
                return true;
            }
            std::string fmt = a.options.count("format") ? a.options.at("format") : "json";
            std::string outDir = a.options.count("out") ? a.options.at("out") : ".";
            bool withMedia = a.options.count("media");
            int limit = 0;
            if (a.options.count("limit")) {
                try { limit = std::stoi(a.options.at("limit")); } catch (...) {}
            }
            bool descOrder = a.options.count("order") && a.options.at("order") == "desc";
            std::string types = a.options.count("types") ? a.options.at("types") : "all";
            long long mediaMaxBytes = -1;
            if (a.options.count("media-max")) {
                try {
                    double mb = std::stod(a.options.at("media-max"));
                    mediaMaxBytes = static_cast<long long>(mb * 1024 * 1024);
                } catch (...) {}
            }
            std::string archive = a.options.count("archive") ? a.options.at("archive") : "";
            // Media needs a session (downloads); --media without one = note.
            bool mediaSession = withMedia && pcore::init() && pcore::loadSavedSession();
            if (withMedia && !mediaSession) {
                std::cout << "note: --media needs a logged-in session — exporting events "
                             "only (mxc URLs are in the JSON)." << std::endl;
            }
            // Type segment filter.
            auto typeAllowed = [&types](const matrix::Event& ev) {
                if (types == "all") return true;
                bool isMedia = false;
                bool isSystem = ev.type == "m.room.member" || ev.type == "m.room.name" ||
                                ev.type == "m.room.topic" || ev.type == "m.room.avatar" ||
                                ev.type == "m.room.create" || ev.type == "m.room.power_levels" ||
                                ev.type == "m.room.join_rules";
                if (ev.type == "m.room.message" && ev.content.is_object()) {
                    auto mt = ev.content.find("msgtype");
                    if (mt != ev.content.end() && mt->is_string()) {
                        std::string m = mt->get<std::string>();
                        isMedia = m == "m.image" || m == "m.video" || m == "m.audio" ||
                                  m == "m.file" || m == "m.sticker";
                    }
                }
                if (ev.type == "m.sticker") isMedia = true;
                bool isMsg = ev.type == "m.room.message" || ev.type == "m.sticker";
                if (types.find("messages") != std::string::npos && isMsg && !isMedia) return true;
                if (types.find("media") != std::string::npos && isMedia) return true;
                if (types.find("system") != std::string::npos && isSystem) return true;
                return false;
            };
            bool wantServer = a.options.count("server");
            bool serverSession = wantServer && pcore::init() && pcore::loadSavedSession();
            if (wantServer && !serverSession) {
                std::cout << "note: --server needs a logged-in session — using the cache "
                             "instead." << std::endl;
            }
            for (const auto& roomQ : a.positional) {
                std::string rid = roomQ;
                std::string rname = roomQ;
                for (const auto& r : st.rooms) {
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == roomQ || id.find(roomQ) != std::string::npos ||
                        name == roomQ || name.find(roomQ) == 0 ||
                        name.find(roomQ) != std::string::npos) {
                        rid = id;
                        rname = name.empty() ? rid : name;
                        break;
                    }
                }
                std::vector<matrix::Event> events;
                if (serverSession) {
                    // Full history from the server: walk /messages?dir=b from
                    // the top (newest) backwards until the beginning.
                    auto& core = pcore::core();
                    auto client = core.client;
                    std::string from = "";
                    int received = 0;
                    std::cout << "dump: " << rname << " — fetching full history from the "
                                 "server..." << std::endl;
                    for (int guard = 0; guard < 2000; ++guard) {
                        auto r = client->getMessages(rid, from, 100);
                        if (!r.ok) {
                            std::cout << "  fetch error: " << r.error.message << std::endl;
                            break;
                        }
                        std::string end;
                        try {
                            auto j = nlohmann::json::parse(r.data);
                            auto chunk = j.value("chunk", nlohmann::json::array());
                            for (const auto& ev : chunk) {
                                matrix::Event e;
                                e.event_id = ev.value("event_id", "");
                                e.sender = ev.value("sender", "");
                                e.type = ev.value("type", "");
                                e.origin_server_ts = ev.value("origin_server_ts", 0LL);
                                auto c = ev.find("content");
                                if (c != ev.end() && c->is_object()) e.content = *c;
                                events.push_back(std::move(e));
                            }
                            received += static_cast<int>(chunk.size());
                            end = j.value("end", "");
                            std::cout << "  received " << received << " events..." << std::endl;
                            if (chunk.empty() || end.empty() || end == from) break;
                        } catch (...) {
                            break;
                        }
                        from = end;
                    }
                } else {
                    events = dbi.getEvents(rid, 50000);
                    std::cout << "dump: " << rname << " — received " << events.size()
                              << " events, processing..." << std::endl;
                }
                // Type segment filter.
                if (types != "all") {
                    std::vector<matrix::Event> filtered;
                    for (const auto& ev : events) {
                        if (typeAllowed(ev)) filtered.push_back(ev);
                    }
                    events = std::move(filtered);
                }
                // --limit N: keep only the last N (the newest) events.
                if (limit > 0 && static_cast<int>(events.size()) > limit) {
                    events.erase(events.begin(), events.end() - limit);
                }
                // --order desc: newest first (asc = chronological, the default).
                if (descOrder) {
                    std::reverse(events.begin(), events.end());
                }
                std::string safeName = rname;
                for (auto& c : safeName) {
                    if (c == '/' || c == '#' || c == '!' || c == ':' || c == ' ') c = '_';
                }
                std::string path = outDir + "/" + safeName + "." + fmt;
                std::ofstream fout(path);
                if (!fout) {
                    std::cout << "  cannot write " << path << std::endl;
                    return true;
                }
                int processed = 0;
                if (fmt == "json") {
                    nlohmann::json j;
                    j["room_id"] = rid;
                    j["name"] = rname;
                    j["events"] = nlohmann::json::array();
                    for (const auto& ev : events) {
                        nlohmann::json e;
                        e["event_id"] = ev.event_id;
                        e["sender"] = ev.sender;
                        e["type"] = ev.type;
                        e["origin_server_ts"] = ev.origin_server_ts;
                        e["content"] = ev.content;
                        j["events"].push_back(e);
                        processed++;
                    }
                    fout << j.dump(1) << std::endl;
                } else if (fmt == "txt") {
                    for (const auto& ev : events) {
                        std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                        std::tm tm{};
                        localtime_r(&t, &tm);
                        char buf[20];
                        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                                      tm.tm_hour, tm.tm_min, tm.tm_sec);
                        fout << "[" << buf << "] " << senderShort(ev.sender) << ": "
                             << eventBody(ev) << std::endl;
                        processed++;
                    }
                } else if (fmt == "html") {
                    fout << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                         << "<title>" << rname << "</title></head><body><h1>"
                         << rname << "</h1>" << std::endl;
                    for (const auto& ev : events) {
                        fout << "<p><b>" << senderShort(ev.sender) << "</b> "
                             << eventBody(ev) << "</p>" << std::endl;
                        processed++;
                    }
                    fout << "</body></html>" << std::endl;
                } else {
                    std::cout << "  unknown format '" << fmt
                              << "' (json|txt|html)" << std::endl;
                    return true;
                }
                fout.close();
                // Media pass: download the attached files into <out>/<name>_media/.
                int mediaSaved = 0;
                if (withMedia && mediaSession) {
                    std::string mediaDir = outDir + "/" + safeName + "_media";
                    std::filesystem::create_directories(mediaDir);
                    auto& core = pcore::core();
                    auto client = core.client;
                    for (const auto& ev : events) {
                        if (!ev.content.is_object()) continue;
                        std::string mxc, key, iv, sha, mime;
                        auto urlIt = ev.content.find("url");
                        if (urlIt != ev.content.end() && urlIt->is_string()) {
                            mxc = urlIt->get<std::string>();
                        }
                        auto fIt = ev.content.find("file");
                        if (mxc.empty() && fIt != ev.content.end() && fIt->is_object()) {
                            auto fu = fIt->find("url");
                            if (fu != fIt->end() && fu->is_string()) mxc = fu->get<std::string>();
                            auto k = fIt->find("key");
                            if (k != fIt->end() && k->is_string()) key = k->get<std::string>();
                            auto ivv = fIt->find("iv");
                            if (ivv != fIt->end() && ivv->is_string()) iv = ivv->get<std::string>();
                            auto h = fIt->find("hashes");
                            if (h != fIt->end() && h->is_object()) {
                                auto s = h->find("sha256");
                                if (s != h->end() && s->is_string()) sha = s->get<std::string>();
                            }
                        }
                        if (mxc.empty()) continue;
                        auto infoIt = ev.content.find("info");
                        if (mediaMaxBytes > 0 && infoIt != ev.content.end() &&
                            infoIt->is_object()) {
                            auto sz = infoIt->find("size");
                            if (sz != infoIt->end() && sz->is_number()) {
                                if (sz->get<long long>() > mediaMaxBytes) {
                                    std::cout << "  media skip (over --media-max): "
                                              << ev.event_id << std::endl;
                                    return true;
                                }
                            }
                        }
                        if (infoIt != ev.content.end() && infoIt->is_object()) {
                            auto m = infoIt->find("mimetype");
                            if (m != infoIt->end() && m->is_string()) mime = m->get<std::string>();
                        }
                        std::string ext = "bin";
                        if (mime == "image/png") ext = "png";
                        else if (mime == "image/jpeg") ext = "jpg";
                        else if (mime == "image/gif") ext = "gif";
                        else if (mime == "image/webp") ext = "webp";
                        else if (mime == "video/mp4") ext = "mp4";
                        else if (mime == "audio/mpeg") ext = "mp3";
                        else if (mime == "audio/ogg") ext = "ogg";
                        std::string local = mediaDir + "/" + ev.event_id.substr(0, 20) + "." + ext;
                        std::vector<uint8_t> bytes;
                        if (!key.empty()) {
                            auto r = client->downloadMediaEncrypted(mxc, key, iv, sha);
                            if (r.ok) bytes = r.data;
                        } else {
                            auto r = client->downloadMedia(mxc, 0, 0);
                            if (r.ok) bytes = r.data;
                        }
                        if (bytes.empty()) {
                            std::cout << "  media skip (download failed): " << ev.event_id
                                      << std::endl;
                            return true;
                        }
                        std::ofstream mout(local, std::ios::binary);
                        if (!mout) continue;
                        mout.write(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
                        mout.close();
                        mediaSaved++;
                    }
                }
                // --archive zip|tar.gz: pack the dump + the media folder.
                if (!archive.empty()) {
                    std::string archPath = outDir + "/" + safeName + "." + archive;
                    if (archive == "zip") {
                        std::string cmd = "cd '" + outDir + "' && zip -r -q '" + safeName
                                        + ".zip' '" + safeName + "." + fmt + "'"
                                        + (withMedia ? " '" + safeName + "_media'" : "")
                                        + " 2>/dev/null";
                        if (std::system(cmd.c_str()) == 0) {
                            std::cout << "  archive: " << archPath << std::endl;
                        } else {
                            std::cout << "  archive failed (is 'zip' installed?)" << std::endl;
                        }
                    } else if (archive == "tar.gz") {
                        std::string cmd = "cd '" + outDir + "' && tar -czf '" + safeName
                                        + ".tar.gz' '" + safeName + "." + fmt + "'"
                                        + (withMedia ? " '" + safeName + "_media'" : "")
                                        + " 2>/dev/null";
                        if (std::system(cmd.c_str()) == 0) {
                            std::cout << "  archive: " << archPath << std::endl;
                        } else {
                            std::cout << "  archive failed" << std::endl;
                        }
                    } else {
                        std::cout << "  unknown --archive '" << archive
                                  << "' (zip|tar.gz)" << std::endl;
                    }
                }
                std::cout << "  done: " << path << " (" << processed << " events"
                          << (withMedia ? ", " + std::to_string(mediaSaved) + " media files)" : ")")
                          << std::endl;
                st.statusNote = "dump: " + rname + " — " + std::to_string(events.size())
                              + " events, " + std::to_string(processed) + " processed -> " + path
                              + (withMedia ? " + " + std::to_string(mediaSaved) + " media" : "");
            }
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        if (a.command == "search" || a.command == "find") {
            if (a.positional.empty()) {
                std::cout << "Usage: find <query> [--limit N] [--sender @u]"
                             " [--since YYYY-MM-DD] [--until YYYY-MM-DD]\n"
                             "  The full-text search over the cached messages"
                             " — the bodies, the mxids, the reply context and"
                             " the matrix.to links.\n";
                return true;
            }
            int limit = 10;
            if (a.options.count("limit")) {
                try { limit = std::stoi(a.options.at("limit")); } catch (...) {}
            }
            int64_t sinceMs = a.options.count("since") ? parseDayMs(a.options.at("since")) : -1;
            int64_t untilMs = a.options.count("until") ? parseDayMs(a.options.at("until")) : -1;
            std::string senderF = a.options.count("sender") ? a.options.at("sender") : "";
            auto hits = dbi.search(a.positional[0], std::max(limit, 5) * 8);
            int shown = 0;
            std::cout << std::endl;
            for (const auto& h : hits) {
                if (shown >= limit) break;
                if (!senderF.empty() && h.value("sender", "") != senderF) continue;
                const int64_t ts = h.value("origin_server_ts", 0LL);
                if (sinceMs > 0 && ts < sinceMs) continue;
                if (untilMs > 0 && ts > untilMs) continue;
                const std::string roomId = h.value("room_id", "");
                const std::string roomName = h.value("room_name", roomId);
                const std::string mxid = h.value("sender", "?");
                const std::string body =
                    h.value("content", nlohmann::json::object()).value("body", "");

                std::time_t t = ts / 1000;
                char tbuf[24];
                std::strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M", std::localtime(&t));
                std::cout << "  \x1b[1m" << roomName << "\x1b[0m  "
                          << senderShort(mxid) << "  (\x1b[36m" << mxid
                          << "\x1b[0m)  " << tbuf << std::endl;
                // The full body, wrapped to the terminal.
                auto lines = wrapText(body, terminalWidth() - 6);
                for (const auto& l : lines) std::cout << "    " << l << std::endl;

                // The reply context: the parent message when present.
                matrix::Event ev;
                if (dbi.getEventById(h.value("event_id", ""), ev) &&
                    ev.content.contains("m.relates_to")) {
                    const auto& rel = ev.content["m.relates_to"];
                    const std::string relType = rel.value("rel_type", "");
                    if (relType == "m.in_reply_to") {
                        const std::string parentId = rel.value("event_id", "");
                        matrix::Event parent;
                        if (dbi.getEventById(parentId, parent)) {
                            std::string pbody = parent.content.value("body", "");
                            if (pbody.size() > 120) pbody = pbody.substr(0, 120) + "...";
                            std::cout << "    \x1b[90m\u21b3 reply to "
                                      << senderShort(parent.sender) << ": "
                                      << pbody << "\x1b[0m" << std::endl;
                        }
                    }
                }
                std::string eid = h.value("event_id", "");
                if (!eid.empty() && eid[0] == '$') eid = eid.substr(1);
                std::cout << "    \x1b[90m\U0001F517 https://matrix.to/#/"
                          << roomId << "/$" << eid
                          << "\x1b[0m" << std::endl << std::endl;
                shown++;
            }
            if (shown == 0) {
                std::cout << "No matches for '" << a.positional[0] << "'"
                          << (senderF.empty() ? "" : " from " + senderF) << std::endl;
            } else {
                std::cout << shown << " match" << (shown == 1 ? "" : "es")
                          << " for '" << a.positional[0] << "'"
                          << (senderF.empty() ? "" : " from " + senderF) << std::endl;
            }
            return true;
        }
        if (a.command == "goto") {
            if (a.positional.empty()) {
                std::cout << "Usage: goto <event_id> | lastread | newest (back to the latest)"
                          << std::endl;
                return true;
            }
            std::string q = a.positional[0];
            if (q == "lastread" || q == "last-read" || q == "unread") {
                // Jump to the last-read position (the local m.fully_read
                // copy), right after the marker.
                if (st.readMarker.empty()) {
                    st.statusNote = "no last-read marker (read <room> sets it)";
                    std::cout << drawFrame(st) << std::flush;
                    return true;
                }
                q = st.readMarker;
            }
            matrix::Event target;
            if (!st.db->getEventById(q, target)) {
                st.statusNote = "event not in the cache: " + q;
                std::cout << drawFrame(st) << std::flush;
                return true;
            }
            if (st.currentRoomId != target.room_id) {
                loadRoomIntoState(st, target.room_id);
            }
            bool inWindow = std::find_if(
                st.messages.begin(), st.messages.end(),
                [&](const matrix::Event& ev) { return ev.event_id == q; }) !=
                st.messages.end();
            if (!inWindow) {
                st.limit = 5000;  // the event is older than the window
                loadRoomIntoState(st, target.room_id);
                inWindow = std::find_if(
                    st.messages.begin(), st.messages.end(),
                    [&](const matrix::Event& ev) { return ev.event_id == q; }) !=
                    st.messages.end();
            }
            if (!inWindow) {
                st.statusNote = "event exists but is outside the loaded window";
                std::cout << drawFrame(st) << std::flush;
                return true;
            }
            st.focusEvent = q;
            int rowIdx = centerRowIndexOf(st, q);
            st.scroll = rowIdx >= 0 ? std::max(0, rowIdx - 12) : 0;
            if (st.mobile) st.mobileTab = 1;
            st.statusNote = "viewing event ‹" + q + "› · 'newest' to return";
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
    return false;
}

} // namespace matrixcli
