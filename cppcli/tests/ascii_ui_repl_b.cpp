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

int asciiReplDispatchB(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "media") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: media <room> <event_id> [--open] [--preview]" << std::endl;
                std::cout << "  --preview renders the image inline (ANSI half-blocks)"
                          << std::endl;
                return 1;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            matrix::Event ev;
            if (!dbi.getEventById(a.positional[1], ev)) {
                std::cout << "Event not found in the cache: " << a.positional[1] << std::endl;
                return 1;
            }
            // The plain "url" or the encrypted "file.url".
            std::string mxc;
            std::string key, iv, sha;
            if (ev.content.is_object()) {
                auto urlIt = ev.content.find("url");
                if (urlIt != ev.content.end() && urlIt->is_string()) mxc = urlIt->get<std::string>();
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
            }
            if (mxc.empty()) {
                std::cout << "No media url in the event." << std::endl;
                return 1;
            }
            bool mediaSession = pcore::init() && pcore::loadSavedSession();
            std::vector<uint8_t> bytes;
            if (!mediaSession) {
                if (a.options.count("preview")) {
                    std::cout << "(no session — previewing the demo sample)"
                              << std::endl;
                } else {
                    std::cout << "media download needs a logged-in session."
                              << std::endl;
                }
            } else {
                auto& core = pcore::core();
                auto client = core.client;
                if (!key.empty()) {
                    auto r = client->downloadMediaEncrypted(mxc, key, iv, sha);
                    if (r.ok) bytes = r.data;
                } else {
                    auto r = client->downloadMedia(mxc, 0, 0);
                    if (r.ok) bytes = r.data;
                }
                if (bytes.empty()) {
                    std::cout << "Download failed; " << std::endl;
                }
            }
            // --preview: render the image inline in the terminal. The CLI has
            // no image library, so the rendering is delegated to a system
            // tool when one exists: chafa (best, ANSI true-color), jp2a
            // (ascii art) or img2txt (libcaca). Nothing is written to disk.
            if (a.options.count("preview")) {
                std::string tmpImg = "/tmp/matrixcli_preview.png";
                if (bytes.empty()) {
                    // No downloadable bytes (demo/local mxc): generate a
                    // sample image so the preview still demonstrates itself.
                    std::cout << "(demo mxc — rendering a sample image instead)"
                              << std::endl;
                    std::string gen = "magick -size 320x160 gradient:orange-red "
                        "-fill white -pointsize 20 -gravity center "
                        "-annotate 0 'demo image' '" + tmpImg
                        + "' 2>/dev/null || convert -size 320x160 "
                        "gradient:orange-red -fill white -pointsize 20 "
                        "-gravity center -annotate 0 'demo image' '" + tmpImg
                        + "' 2>/dev/null";
                    std::system(gen.c_str());
                } else {
                    std::ofstream pout(tmpImg, std::ios::binary);
                    pout.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
                }
                int cols = std::max(30, terminalWidthImpl() - 4);
                int rows = std::max(10, cols / 3);
                std::string cmd;
                if (std::system("which chafa >/dev/null 2>&1") == 0) {
                    cmd = "chafa --format symbols --size " + std::to_string(cols) + "x"
                        + std::to_string(rows) + " '" + tmpImg + "' 2>/dev/null";
                } else if (std::system("which jp2a >/dev/null 2>&1") == 0) {
                    cmd = "jp2a --width=" + std::to_string(cols) + " '" + tmpImg
                        + "' 2>/dev/null";
                } else if (std::system("which img2txt >/dev/null 2>&1") == 0) {
                    cmd = "img2txt -W " + std::to_string(cols) + " -H "
                        + std::to_string(rows) + " '" + tmpImg + "' 2>/dev/null";
                } else {
                    std::cout << "(no image renderer found — install chafa for inline"
                                 " previews; the file is saved below instead)"
                              << std::endl;
                }
                if (!cmd.empty()) std::system(cmd.c_str());
                std::remove(tmpImg.c_str());
            }
            std::string fn = "media_" + a.positional[1].substr(0, 12) + ".bin";
            std::ofstream out(fn, std::ios::binary);
            if (!out) {
                std::cout << "Cannot write " << fn << std::endl;
                return 1;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            out.close();
            st.statusNote = "media saved: " + fn + " (" + std::to_string(bytes.size()) + " bytes)";
            std::cout << "Saved " << bytes.size() << " bytes to " << fn << std::endl;
            if (a.options.count("open")) {
                std::string cmd = "xdg-open '" + fn + "' 2>/dev/null &";
                std::system(cmd.c_str());
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- dump: export the room like Element Web ----
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
                return 1;
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
                    continue;
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
                        fout << "[" << buf << "] " << senderShortImpl(ev.sender) << ": "
                             << eventBody(ev) << std::endl;
                        processed++;
                    }
                } else if (fmt == "html") {
                    fout << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                         << "<title>" << rname << "</title></head><body><h1>"
                         << rname << "</h1>" << std::endl;
                    for (const auto& ev : events) {
                        fout << "<p><b>" << senderShortImpl(ev.sender) << "</b> "
                             << eventBody(ev) << "</p>" << std::endl;
                        processed++;
                    }
                    fout << "</body></html>" << std::endl;
                } else {
                    std::cout << "  unknown format '" << fmt
                              << "' (json|txt|html)" << std::endl;
                    continue;
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
                                    continue;
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
                            continue;
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
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- profile: show a user's profile (display name, avatar) ----
        if (a.command == "profile") {
            if (a.positional.empty()) {
                std::cout << "Usage: profile <@user:server>  (see 'members' for the"
                             " full ids)" << std::endl;
                return 1;
            }
            auto cliHandler = CommandRegistry::instance().findCli("profile");
            if (!cliHandler) {
                std::cout << "profile not available in this build." << std::endl;
                return 1;
            }
            cliHandler(a);
            return 1;
        }
        // ---- members: list the room's members with FULL ids ----
        if (a.command == "members") {
            // The user-list layout setting (a room query would never match
            // these words, so they are safe to intercept).
            if (a.positional.size() >= 1 &&
                (a.positional[0] == "horizontal" || a.positional[0] == "list" ||
                 a.positional[0] == "vertical" || a.positional[0] == "auto")) {
                std::string v = a.positional[0];
                if (v == "horizontal") st.membersMode = 1;
                else if (v == "list" || v == "vertical") st.membersMode = 2;
                else st.membersMode = 0;
                dbi.setSetting("members_mode", std::to_string(st.membersMode));
                st.statusNote = std::string("members ") +
                                (st.membersMode == 1 ? "horizontal" :
                                 st.membersMode == 2 ? "vertical list" : "auto");
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || id.find(q) != std::string::npos ||
                    name == q || name.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            auto evs = st.db->getEvents(roomId, 500);
            std::vector<std::string> seen;
            for (const auto& ev : evs) {
                if (std::find(seen.begin(), seen.end(), ev.sender) == seen.end()) {
                    seen.push_back(ev.sender);
                }
            }
            for (const auto& m : seen) {
                auto pit = st.presence.find(m);
                std::string letter = pit != st.presence.end() ? "[" + pit->second + "] " : "";
                std::cout << "  " << letter << m << "  (profile <@user>)" << std::endl;
            }
            if (seen.empty()) std::cout << "(no members in the cache for " << roomId << ")"
                                        << std::endl;
            return 1;
        }
        // ---- panel <left|center|right> <off|on|W> ----
        if (a.command == "panel") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: panel <left|right> <off|on|width>" << std::endl;
                return 1;
            }
            std::string which = a.positional[0];
            std::string v = a.positional[1];
            int w = -1;
            if (v == "off") w = 0;
            else if (v == "on") w = -1;
            else { try { w = std::stoi(v); } catch (...) { w = -1; } }
            if (which == "left") st.leftPanelW = w;
            else if (which == "right") st.rightPanelW = w;
            else {
                std::cout << "Usage: panel <left|right> <off|on|width>" << std::endl;
                return 1;
            }
            st.statusNote = std::string("panel ") + which + " = " + v;
            dbi.setSetting(which == "left" ? "panel_left" : "panel_right",
                           std::to_string(w));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- panel auto on|off: size the panels to the content ----
        if (a.command == "panel" && a.positional.size() >= 2 &&
            a.positional[0] == "auto") {
            st.autoPanels = (a.positional[1] != "off" && a.positional[1] != "0");
            dbi.setSetting("panel_auto", st.autoPanels ? "1" : "0");
            st.statusNote = std::string("panel auto ") +
                            (st.autoPanels ? "on (sized to content)" : "off (fixed)");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- emoji on|off: emoji glyphs or ASCII fallbacks ----
        if (a.command == "emoji") {
            if (a.positional.empty() || a.positional[0] == "on") st.showEmoji = true;
            else st.showEmoji = false;
            st.statusNote = std::string("emoji ") + (st.showEmoji ? "on" : "off (ASCII)");
            dbi.setSetting("emoji", st.showEmoji ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- images on|off: full image cards ----
        if (a.command == "images") {
            if (a.positional.empty() || a.positional[0] == "on") st.showImages = true;
            else st.showImages = false;
            st.statusNote = std::string("images ") + (st.showImages ? "full cards" : "compact");
            dbi.setSetting("images", st.showImages ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- raw <room> <event_id>: the event's raw JSON ----
        if (a.command == "raw") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: raw <room> <event_id>" << std::endl;
                return 1;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            matrix::Event ev;
            if (!dbi.getEventById(a.positional[1], ev)) {
                std::cout << "Event not found in the cache: " << a.positional[1]
                          << std::endl;
                return 1;
            }
            nlohmann::json j;
            j["event_id"] = ev.event_id;
            j["room_id"] = roomId;
            j["sender"] = ev.sender;
            j["type"] = ev.type;
            j["origin_server_ts"] = ev.origin_server_ts;
            j["content"] = ev.content;
            std::cout << j.dump(2) << std::endl;
            return 1;
        }
        // ---- about: version + ASCII logo ----
        if (a.command == "about") {
            printAbout(st.proxyLabel, st.accountLabel);
            return 1;
        }
        // ---- ids on|off: show event ids next to the messages ----
        if (a.command == "ids") {
            if (a.positional.empty() || a.positional[0] == "on") st.showIds = true;
            else if (a.positional[0] == "off") st.showIds = false;
            else st.showIds = true;
            st.statusNote = std::string("event ids ") + (st.showIds ? "shown" : "hidden");
            dbi.setSetting("ids", st.showIds ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- time [full|sec|off]: message time with seconds ----
        if (a.command == "time") {
            if (a.positional.empty()) {
                std::cout << "Usage: time full | time sec (HH:MM:SS) | time off (HH:MM)"
                          << std::endl;
                return 1;
            }
            std::string v = a.positional[0];
            if (v == "off") st.showSeconds = false;
            else st.showSeconds = true;  // full / sec / anything = seconds on
            st.statusNote = std::string("time ") + (st.showSeconds ? "HH:MM:SS" : "HH:MM");
            dbi.setSetting("time_full", st.showSeconds ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- timeside left|right: the chat time on the left or right ----
        if (a.command == "timeside") {
            std::string v = a.positional.empty() ? "" : a.positional[0];
            if (v != "left" && v != "right") {
                std::cout << "Usage: timeside left | timeside right" << std::endl;
                return 1;
            }
            st.timeRight = (v == "right");
            dbi.setSetting("time_side", v);
            st.statusNote = "time on the " + v;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- msgline inline|newline: message next to or under the time ----
        if (a.command == "msgline") {
            std::string v = a.positional.empty() ? "" : a.positional[0];
            if (v != "inline" && v != "newline") {
                std::cout << "Usage: msgline inline | msgline newline" << std::endl;
                return 1;
            }
            st.msgNewline = (v == "newline");
            dbi.setSetting("msg_line", v);
            st.statusNote = "message " + v;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }

        // ---- agent <prompt>: the local coding agent (opencode-style) ----
    return 0;
}

} // namespace matrixcli
