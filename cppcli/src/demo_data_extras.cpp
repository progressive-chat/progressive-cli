// src/demo_tui.cpp — the demo data, the demo REPL and the ncurses TUI
// (split out of main.cpp so the compilation stays incremental-friendly).
#include "commands.hpp"
#include "config.hpp"
#include "cli/args.hpp"
#include "core/http_client.hpp"
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/client_utils.hpp"
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace matrixcli;

static std::string demoName(const char* s) {
    std::string n = s;
    if (!n.empty() && n[0] == '@') {
        auto colon = n.find(':');
        if (colon != std::string::npos) n = n.substr(1, colon - 1);
        else n = n.substr(1);
    }
    if (n == "you") return n;
    if (!n.empty()) n[0] = std::toupper(static_cast<unsigned char>(n[0]));
    return n;
}

// populateDemoDataExtras: the membership/invite block, the reply
// chains, the fresh activity, the summary numbers.
void populateDemoDataExtras(matrixcli::db::Database& dbi) {
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Membership events: joined/left rows (the ui renders them as system lines).
    {
        auto shortName = [](const char* s) {
            std::string out = s;
            if (!out.empty() && out[0] == '@') {
                auto colon = out.find(':');
                if (colon != std::string::npos) out = out.substr(1, colon - 1);
                else out = out.substr(1);
            }
            return out;
        };
        struct { const char* room; const char* sender; const char* ms; } mem[] = {
            {"!general:demo.local", "@alice", "join"},
            {"!general:demo.local", "@bob", "join"},
            {"!general:demo.local", "@carol", "join"},
            {"!general:demo.local", "@charlie", "join"},
            {"!general:demo.local", "@dave", "join"},
            {"!general:demo.local", "@erin", "join"},
            {"!general:demo.local", "@frank", "join"},
            {"!general:demo.local", "@grace", "join"},
            {"!general:demo.local", "@you", "join"},
            {"!random:demo.local", "@bob", "join"},
            {"!dev:demo.local", "@bob", "leave"},
            {"!design:demo.local", "@carol", "join"},
            {"!design:demo.local", "@dave", "join"},
            {"!music:demo.local", "@erin", "join"},
            {"!games:demo.local", "@frank", "join"},
            {"!games:demo.local", "@you", "join"},
            {"!science:demo.local", "@grace", "join"},
            {"!help:demo.local", "@you", "join"},
            {"!offtopic:demo.local", "@erin", "join"},
            {"!announce:demo.local", "@you", "join"},
            {"!linux:demo.local", "@dave", "join"},
            {"!linux:demo.local", "@grace", "join"},
            {"!linux:demo.local", "@you", "join"},
            {"!crypto:demo.local", "@frank", "join"},
            {"!photography:demo.local", "@carol", "join"},
            {"!travel:demo.local", "@erin", "join"},
            {"!travel:demo.local", "@you", "join"},
            {"!food:demo.local", "@bob", "join"},
            {"!books:demo.local", "@grace", "join"},
            {"!fitness:demo.local", "@you", "join"},
            {"!movies:demo.local", "@alice", "join"},
            {"!programming:demo.local", "@dave", "join"},
            {"!programming:demo.local", "@you", "join"},
            {"!rust:demo.local", "@frank", "join"},
            {"!matrix:demo.local", "@alice", "join"},
            {"!matrix:demo.local", "@you", "join"},
            {"!meta:demo.local", "@you", "join"},
            {"!space_tech:demo.local", "@you", "join"},
            {"!space_social:demo.local", "@you", "join"},
            // Open invites FOR @you (sent by other members) — the header
            // shows the invite count, the timeline the invite rows.
            {"!design:demo.local", "@alice", "invite"},
            {"!crypto:demo.local", "@bob", "invite"},
            {"!books:demo.local", "@grace", "invite"},
            {"!travel:demo.local", "@erin", "invite"},
            {"!ai-art:demo.local", "@dave", "invite"},
               {"!sports:demo.local", "@you", "join"},
            {"!sports:demo.local", "@bob", "join"},
            {"!sports:demo.local", "@erin", "join"},
            {"!chess:demo.local", "@you", "join"},
            {"!chess:demo.local", "@charlie", "join"},
            {"!retro-gaming:demo.local", "@you", "join"},
            {"!retro-gaming:demo.local", "@carol", "join"},
            {"!hardware:demo.local", "@you", "join"},
            {"!hardware:demo.local", "@dave", "join"},
            {"!hardware:demo.local", "@alice", "join"},
            {"!distro-talk:demo.local", "@you", "join"},
            {"!distro-talk:demo.local", "@erin", "join"},
            {"!shell:demo.local", "@you", "join"},
            {"!shell:demo.local", "@frank", "join"},
            {"!editors:demo.local", "@you", "join"},
            {"!editors:demo.local", "@grace", "join"},
            {"!editors:demo.local", "@carol", "join"},
            {"!git:demo.local", "@you", "join"},
            {"!git:demo.local", "@alice", "join"},
            {"!dotfiles:demo.local", "@you", "join"},
            {"!dotfiles:demo.local", "@bob", "join"},
            {"!selfhosting:demo.local", "@you", "join"},
            {"!selfhosting:demo.local", "@charlie", "join"},
            {"!selfhosting:demo.local", "@frank", "join"},
            {"!homelab:demo.local", "@you", "join"},
            {"!homelab:demo.local", "@carol", "join"},
            {"!security:demo.local", "@you", "join"},
            {"!security:demo.local", "@dave", "join"},
            {"!privacy:demo.local", "@you", "join"},
            {"!privacy:demo.local", "@erin", "join"},
            {"!privacy:demo.local", "@bob", "join"},
            {"!networking:demo.local", "@you", "join"},
            {"!networking:demo.local", "@frank", "join"},
            {"!databases:demo.local", "@you", "join"},
            {"!databases:demo.local", "@grace", "join"},
            {"!webdev:demo.local", "@you", "join"},
            {"!webdev:demo.local", "@alice", "join"},
            {"!webdev:demo.local", "@dave", "join"},
            {"!frontend:demo.local", "@you", "join"},
            {"!frontend:demo.local", "@bob", "join"},
            {"!backend:demo.local", "@you", "join"},
            {"!backend:demo.local", "@charlie", "join"},
            {"!ml:demo.local", "@you", "join"},
            {"!ml:demo.local", "@carol", "join"},
            {"!ml:demo.local", "@grace", "join"},
            {"!ai-art:demo.local", "@you", "join"},
            {"!ai-art:demo.local", "@dave", "join"},
            {"!astronomy:demo.local", "@you", "join"},
            {"!astronomy:demo.local", "@erin", "join"},
            {"!physics:demo.local", "@you", "join"},
            {"!physics:demo.local", "@frank", "join"},
            {"!physics:demo.local", "@charlie", "join"},
            {"!chemistry:demo.local", "@you", "join"},
            {"!chemistry:demo.local", "@grace", "join"},
            {"!biology:demo.local", "@you", "join"},
            {"!biology:demo.local", "@alice", "join"},
            {"!math:demo.local", "@you", "join"},
            {"!math:demo.local", "@bob", "join"},
            {"!math:demo.local", "@erin", "join"},
            {"!history:demo.local", "@you", "join"},
            {"!history:demo.local", "@charlie", "join"},
            {"!philosophy:demo.local", "@you", "join"},
            {"!philosophy:demo.local", "@carol", "join"},
            {"!languages:demo.local", "@you", "join"},
            {"!languages:demo.local", "@dave", "join"},
            {"!languages:demo.local", "@alice", "join"},
            {"!writing:demo.local", "@you", "join"},
            {"!writing:demo.local", "@erin", "join"},
            {"!poetry:demo.local", "@you", "join"},
            {"!poetry:demo.local", "@frank", "join"},
            {"!art:demo.local", "@you", "join"},
            {"!art:demo.local", "@grace", "join"},
            {"!art:demo.local", "@carol", "join"},
            {"!pixelart:demo.local", "@you", "join"},
            {"!pixelart:demo.local", "@alice", "join"},
            {"!music-production:demo.local", "@you", "join"},
            {"!music-production:demo.local", "@bob", "join"},
            {"!synth:demo.local", "@you", "join"},
            {"!synth:demo.local", "@charlie", "join"},
            {"!synth:demo.local", "@frank", "join"},
            {"!jazz:demo.local", "@you", "join"},
            {"!jazz:demo.local", "@carol", "join"},
            {"!metal:demo.local", "@you", "join"},
            {"!metal:demo.local", "@dave", "join"},
            {"!classical:demo.local", "@you", "join"},
            {"!classical:demo.local", "@erin", "join"},
            {"!classical:demo.local", "@bob", "join"},
            {"!techno:demo.local", "@you", "join"},
            {"!techno:demo.local", "@frank", "join"},
            {"!dnb:demo.local", "@you", "join"},
            {"!dnb:demo.local", "@grace", "join"},
            {"!hiking:demo.local", "@you", "join"},
            {"!hiking:demo.local", "@alice", "join"},
            {"!hiking:demo.local", "@dave", "join"},
            {"!camping:demo.local", "@you", "join"},
            {"!camping:demo.local", "@bob", "join"},
            {"!cycling:demo.local", "@you", "join"},
            {"!cycling:demo.local", "@charlie", "join"},
            {"!running:demo.local", "@you", "join"},
            {"!running:demo.local", "@carol", "join"},
            {"!running:demo.local", "@grace", "join"},
            {"!climbing:demo.local", "@you", "join"},
            {"!climbing:demo.local", "@dave", "join"},
            {"!yoga:demo.local", "@you", "join"},
            {"!yoga:demo.local", "@erin", "join"},
            {"!vegan:demo.local", "@you", "join"},
            {"!vegan:demo.local", "@frank", "join"},
            {"!vegan:demo.local", "@charlie", "join"},
            {"!baking:demo.local", "@you", "join"},
            {"!baking:demo.local", "@grace", "join"},
            {"!coffee:demo.local", "@you", "join"},
            {"!coffee:demo.local", "@alice", "join"},
            {"!tea:demo.local", "@you", "join"},
            {"!tea:demo.local", "@bob", "join"},
            {"!tea:demo.local", "@erin", "join"},
            {"!beer:demo.local", "@you", "join"},
            {"!beer:demo.local", "@charlie", "join"},
            {"!wine:demo.local", "@you", "join"},
            {"!wine:demo.local", "@carol", "join"},
            {"!boardgames:demo.local", "@you", "join"},
            {"!boardgames:demo.local", "@dave", "join"},
            {"!boardgames:demo.local", "@alice", "join"},
            {"!podcasts:demo.local", "@you", "join"},
            {"!podcasts:demo.local", "@erin", "join"},
            {"!memes:demo.local", "@you", "join"},
            {"!memes:demo.local", "@frank", "join"},
            {"!diy:demo.local", "@you", "join"},
            {"!diy:demo.local", "@grace", "join"},
            {"!diy:demo.local", "@carol", "join"},
            {"!finance:demo.local", "@you", "join"},
            {"!finance:demo.local", "@alice", "join"},
        };
        int inviteIdx = 0;  // resets on every rebuild
        for (auto& m : mem) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.member";
            nlohmann::json content;
            content["membership"] = m.ms;
            content["displayname"] = demoName(m.sender);
            // Invites carry the invited user and a reason, shown in the
            // timeline ("alice invited you — reason").
            if (strcmp(m.ms, "invite") == 0) {
                ev.state_key = "@you:demo.local";
                const char* reason = m.room;
                if (strcmp(m.room, "!design:demo.local") == 0)
                    reason = "the design crew wants you!";
                else if (strcmp(m.room, "!crypto:demo.local") == 0)
                    reason = "we discuss the latest coins";
                else if (strcmp(m.room, "!books:demo.local") == 0)
                    reason = "book club needs new readers";
                else if (strcmp(m.room, "!travel:demo.local") == 0)
                    reason = "we're planning the summer trip";
                else if (strcmp(m.room, "!ai-art:demo.local") == 0)
                    reason = "your generations are amazing";
                content["reason"] = reason;
                // Each invite gets its own recent age (30m, 2h, 5h, 1d,
                // 3d) so the remembered invitation dates are easy to see.
                static const int64_t inviteAges[] = {
                    30 * 60 * 1000LL, 2 * 3600 * 1000LL, 5 * 3600 * 1000LL,
                    86400 * 1000LL, 3 * 86400 * 1000LL,
                };
                int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int idx = inviteIdx < 5 ? inviteIdx : 4;
                inviteIdx++;
                ev.origin_server_ts = nowMs - inviteAges[idx];
            }
            ev.content = content;
            if (strcmp(m.ms, "invite") != 0) ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            ts -= 60;
        }
        // An OLD open invite: ten days old, so the room list can show the
        // remembered invitation date ("invited 10d ago") next to the
        // fresh ones.
        {
            matrix::Event ev;
            ev.event_id = "$demo_old_invite";
            ev.room_id = "!food:demo.local";
            ev.sender = "@bob";
            ev.type = "m.room.member";
            ev.state_key = "@you:demo.local";
            ev.content = {{"membership", "invite"},
                          {"displayname", "Bob"},
                          {"reason", "we miss your recipes"}};
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ev.origin_server_ts = nowMs - 10LL * 86400 * 1000;
            dbi.insertEvent(ev);
        }
    }

    // The join rules of the invited rooms — the invite rows show whether
    // the room is open (public: anyone can preview/join) or closed
    // (invite-only / restricted).
    {
        struct { const char* room; const char* rule; } rules[] = {
            {"!design:demo.local", "public"},
            {"!travel:demo.local", "public"},
            {"!crypto:demo.local", "invite"},
            {"!books:demo.local", "invite"},
            {"!food:demo.local", "invite"},
            {"!ai-art:demo.local", "restricted"},
        };
        for (const auto& r : rules) {
            matrix::Event ev;
            ev.event_id = "$demo_jr_" + std::string(r.room);
            ev.room_id = r.room; ev.sender = "@alice";
            ev.type = "m.room.join_rules";
            ev.content = {{"join_rule", r.rule}};
            ev.origin_server_ts = ts - 5LL * 86400 * 1000;
            dbi.insertEvent(ev);
        }
    }

    // A real THREAD (m.thread relation): a root message + replies. The
    // ASCII UI and the view command render these with the thread marker.
    {
        std::string rootId = "$demo_" + std::to_string(ts);
        matrix::Event tRoot;
        tRoot.event_id = rootId;
        tRoot.room_id = "!dev:demo.local"; tRoot.sender = "@alice";
        tRoot.type = "m.room.message";
        tRoot.content = {{"body", "Let's plan the v0.5 release"},
                         {"msgtype", "m.text"}};
        tRoot.origin_server_ts = ts;
        dbi.insertEvent(tRoot);
        ts -= 60;

        struct { const char* sender; const char* body; } reps[] = {
            {"@charlie", "Agreed — how about Friday?"},
            {"@alice", "Friday works for me"},
            {"@bob", "Let me check my calendar, ETA tomorrow"},
        };
        for (auto& rp : reps) {
            matrix::Event tr;
            tr.event_id = "$demo_" + std::to_string(ts);
            tr.room_id = "!dev:demo.local"; tr.sender = rp.sender;
            tr.type = "m.room.message";
            tr.content = {{"body", rp.body}, {"msgtype", "m.text"},
                          {"m.relates_to", {{"m.thread", {{"event_id", rootId}}}}}};
            tr.origin_server_ts = ts;
            dbi.insertEvent(tr);
            ts -= 60;
        }
    }

    // Multilevel reply chain (element-web style): bob replies to alice's
    // "Welcome!", alice replies to bob's reply (reply-of-reply). The fallback
    // "> quote" block is what real clients embed in reply bodies.
    {
        std::string alice_welcome = "$demo_" + std::to_string(ts + 8 * 60);
        matrix::Event r1;
        r1.event_id = "$demo_" + std::to_string(ts);
        r1.room_id = "!general:demo.local"; r1.sender = "@bob";
        r1.type = "m.room.message";
        r1.content = {{"body",
            "> <@alice:demo.local> Welcome! This is progressive-cli — a terminal Matrix client.\n\n"
            "It even renders reply chains!"},
            {"msgtype", "m.text"},
            {"m.relates_to", {{"m.in_reply_to", {{"event_id", alice_welcome}}}}}};
        r1.origin_server_ts = ts;
        dbi.insertEvent(r1);
        std::string bob_reply = r1.event_id;
        ts -= 60;

        matrix::Event r2;
        r2.event_id = "$demo_" + std::to_string(ts);
        r2.room_id = "!general:demo.local"; r2.sender = "@alice";
        r2.type = "m.room.message";
        r2.content = {{"body",
            "> <@bob:demo.local> It even renders reply chains!\n\n"
            "…and the reply to that too."},
            {"msgtype", "m.text"},
            {"m.relates_to", {{"m.in_reply_to", {{"event_id", bob_reply}}}}}};
        r2.origin_server_ts = ts;
        dbi.insertEvent(r2);
        ts -= 60;
    }

    // Fresh activity: each room gets a recent message so the room list
    // shows times (HH:MM) and sorts by recency (a few rooms keep their
    // older last messages for the mixed date/time look). Every body is
    // unique per room — no two rooms share the same last message.
    {
        struct { const char* room; const char* sender; const char* body; } fresh[] = {
            {"!general:demo.local", "@bob", "Markdown demo: **bold text**, *italic*, `inline code`, [a link](https://matrix.org) and a raw https://matrix.org\n\n# A header\n\n- a bullet\n- another bullet\n- [x] done task\n- [ ] open task\n\n1. first step\n2. second step\n\n> a quoted line\n\n```cpp\n#include <iostream>\n\nint main() {\n    // a highlighted comment\n    const char* s = \"hello markdown\";\n    std::cout << s << std::endl;\n    return 0;\n}\n```"},
            {"!general:demo.local", "@bob", "The maintainers dropped fresh links in this room."},
            {"!dev:demo.local", "@dave", "We is discussing a few thoughts in this room."},
            {"!random:demo.local", "@grace", "The team keeps sharing fresh links in this room."},
            {"!design:demo.local", "@alice", "The crew compiled the demo build in this room."},
            {"!music:demo.local", "@erin", "Half of us compiled a question in this room."},
            {"!games:demo.local", "@alice", "Our group is discussing the demo build in this room."},
            {"!science:demo.local", "@carol", "I is discussing the release notes in this room."},
            {"!offtopic:demo.local", "@frank", "Everyone compiled some findings in this room."},
            {"!announcements:demo.local", "@bob", "Half of us compiled the release notes in this room."},
            {"!help:demo.local", "@dave", "The maintainers uploaded useful tips in this room."},
            {"!linux:demo.local", "@grace", "We is discussing great ideas in this room."},
            {"!crypto:demo.local", "@alice", "The team posted useful tips in this room."},
            {"!photography:demo.local", "@erin", "The crew posted fresh links in this room."},
            {"!travel:demo.local", "@alice", "Half of us uploaded the roadmap in this room."},
            {"!food:demo.local", "@carol", "Our group uploaded fresh links in this room."},
            {"!fitness:demo.local", "@frank", "I posted the roadmap in this room."},
            {"!movies:demo.local", "@bob", "Everyone posted the latest updates in this room."},
            {"!programming:demo.local", "@dave", "Half of us keeps sharing the demo build in this room."},
            {"!rust:demo.local", "@grace", "The maintainers uploaded the latest updates in this room."},
            {"!matrix:demo.local", "@charlie", "We dropped some findings in this room."},
            {"!sports:demo.local", "@erin", "The team keeps sharing a few thoughts in this room."},
            {"!chess:demo.local", "@alice", "The crew keeps sharing some findings in this room."},
            {"!retro-gaming:demo.local", "@carol", "The maintainers compiled great ideas in this room."},
            {"!hardware:demo.local", "@frank", "Our group dropped a question in this room."},
            {"!distro-talk:demo.local", "@bob", "I is discussing great ideas in this room."},
            {"!shell:demo.local", "@dave", "Everyone is discussing the roadmap in this room."},
            {"!editors:demo.local", "@grace", "Half of us compiled meeting notes in this room."},
            {"!git:demo.local", "@charlie", "The maintainers compiled the roadmap in this room."},
            {"!dotfiles:demo.local", "@erin", "We is discussing meeting notes in this room."},
            {"!selfhosting:demo.local", "@alice", "The team is discussing new screenshots in this room."},
            {"!homelab:demo.local", "@carol", "The crew posted the latest updates in this room."},
            {"!security:demo.local", "@frank", "The maintainers compiled new screenshots in this room."},
            {"!privacy:demo.local", "@bob", "Our group uploaded a few thoughts in this room."},
            {"!networking:demo.local", "@dave", "I uploaded some findings in this room."},
            {"!databases:demo.local", "@grace", "Everyone posted a few thoughts in this room."},
            {"!webdev:demo.local", "@charlie", "Half of us posted a question in this room."},
            {"!frontend:demo.local", "@erin", "The maintainers uploaded the demo build in this room."},
            {"!backend:demo.local", "@alice", "We uploaded a question in this room."},
            {"!ml:demo.local", "@carol", "The team dropped meeting notes in this room."},
            {"!ai-art:demo.local", "@frank", "The crew posted the release notes in this room."},
            {"!astronomy:demo.local", "@bob", "The maintainers keeps sharing meeting notes in this room."},
            {"!physics:demo.local", "@dave", "Our group dropped the release notes in this room."},
            {"!chemistry:demo.local", "@grace", "I dropped useful tips in this room."},
            {"!biology:demo.local", "@charlie", "Everyone is discussing new screenshots in this room."},
            {"!math:demo.local", "@erin", "Half of us keeps sharing useful tips in this room."},
            {"!history:demo.local", "@alice", "Our group compiled fresh links in this room."},
            {"!philosophy:demo.local", "@carol", "We compiled a few thoughts in this room."},
            {"!languages:demo.local", "@frank", "The team is discussing fresh links in this room."},
            {"!writing:demo.local", "@bob", "The crew is discussing the demo build in this room."},
            {"!poetry:demo.local", "@dave", "The maintainers compiled the latest updates in this room."},
            {"!art:demo.local", "@grace", "Our group compiled the demo build in this room."},
            {"!pixelart:demo.local", "@charlie", "I uploaded the release notes in this room."},
            {"!music-production:demo.local", "@erin", "Everyone is discussing some findings in this room."},
            {"!synth:demo.local", "@alice", "Half of us posted the release notes in this room."},
            {"!jazz:demo.local", "@carol", "Our group compiled some findings in this room."},
            {"!metal:demo.local", "@frank", "We uploaded great ideas in this room."},
            {"!classical:demo.local", "@bob", "The team uploaded useful tips in this room."},
            {"!techno:demo.local", "@dave", "The crew posted great ideas in this room."},
            {"!dnb:demo.local", "@grace", "The maintainers posted the roadmap in this room."},
            {"!hiking:demo.local", "@charlie", "Our group keeps sharing fresh links in this room."},
            {"!camping:demo.local", "@erin", "I dropped the roadmap in this room."},
            {"!cycling:demo.local", "@alice", "Everyone dropped the latest updates in this room."},
            {"!running:demo.local", "@carol", "Half of us keeps sharing new screenshots in this room."},
            {"!climbing:demo.local", "@frank", "Our group keeps sharing the latest updates in this room."},
            {"!vegan:demo.local", "@bob", "We compiled some findings in this room."},
            {"!baking:demo.local", "@dave", "The team dropped a few thoughts in this room."},
            {"!coffee:demo.local", "@grace", "The crew is discussing some findings in this room."},
            {"!beer:demo.local", "@charlie", "The maintainers is discussing the release notes in this room."},
            {"!boardgames:demo.local", "@erin", "We compiled a question in this room."},
            {"!podcasts:demo.local", "@alice", "I compiled great ideas in this room."},
            {"!memes:demo.local", "@carol", "Everyone is discussing a question in this room."},
            {"!diy:demo.local", "@frank", "Half of us is discussing meeting notes in this room."},
            {"!finance:demo.local", "@bob", "Our group posted the roadmap in this room."},
            {"!dm_alice:demo.local", "@dave", "We compiled meeting notes in this room."},
            {"!dm_bob:demo.local", "@grace", "The team uploaded new screenshots in this room."},
            {"!dm_carol:demo.local", "@charlie", "The crew is discussing useful tips in this room."},
            {"!dm_dave:demo.local", "@erin", "The maintainers posted new screenshots in this room."},
        };
        // A few rooms keep older last messages (days ago) so the room
        // list mixes "HH:MM" and "MM-DD" timestamps, in recency order
        // (their older events sort them to the bottom of the list).
        struct { const char* room; int days; } older[] = {
            {"!podcasts:demo.local", 1},
            {"!memes:demo.local", 2},
            {"!diy:demo.local", 3},
            {"!finance:demo.local", 5},
            {"!dm_carol:demo.local", 8},
        };
        int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int fi = 0; fi < int(sizeof(fresh) / sizeof(fresh[0])); ++fi) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(t0 - int64_t(fi) * 7);
            ev.room_id = fresh[fi].room; ev.sender = fresh[fi].sender;
            ev.type = "m.room.message";
            ev.content = {{"msgtype", "m.text"}, {"body", fresh[fi].body}};
            ev.origin_server_ts = t0 - int64_t(fi) * 137000 - int64_t(fi % 5) * 23000;
            for (const auto& o : older) {
                if (std::string(o.room) == fresh[fi].room) {
                    ev.origin_server_ts = t0 - int64_t(o.days) * 86400000LL
                                        - int64_t(fi) * 137000 - int64_t(fi % 5) * 23000;
                    break;
                }
            }
            dbi.insertEvent(ev);
        }
    }

    // More users and a livelier #general: fresh members join, a long
    // conversation runs over the last hour (mentions, URLs, a file, a
    // second poll with votes, reactions and a reply).
    {
        int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t gt = t0 - 90 * 60000;
        auto ins = [&](const char* sender, const char* body) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(gt);
            ev.room_id = "!general:demo.local"; ev.sender = sender;
            ev.type = "m.room.message";
            ev.content = {{"msgtype", "m.text"}, {"body", body}};
            ev.origin_server_ts = gt;
            dbi.insertEvent(ev);
            gt -= 120000;
        };
        // The new members join.
        const char* newbies[] = {"@heidi", "@ivan", "@julia", "@kate"};
        for (const char* nb : newbies) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(gt);
            ev.room_id = "!general:demo.local"; ev.sender = nb;
            ev.type = "m.room.member";
            ev.content = {{"membership", "join"},
                          {"displayname", demoName(nb)}};
            ev.origin_server_ts = gt;
            dbi.insertEvent(ev);
            gt -= 60000;
        }
        // Members from other servers — they give the permalink via list
        // real servers (matrix.org, element.io, mozilla.org).
        {
            struct { const char* sender; const char* name; } ext[] = {
                {"@alice:matrix.org", "Alice"},
                {"@mallory:matrix.org", "mallory"},
                {"@trent:element.io", "trent"},
                {"@wendy:mozilla.org", "wendy"},
            };
            for (auto& e : ext) {
                matrix::Event ev;
                ev.event_id = "$demo_" + std::to_string(gt);
                ev.room_id = "!general:demo.local"; ev.sender = e.sender;
                ev.type = "m.room.member";
                ev.content = nlohmann::json::object();
                ev.content["membership"] = "join";
                ev.content["displayname"] = demoName(e.name);
                ev.origin_server_ts = gt;
                dbi.insertEvent(ev);
                gt -= 60000;
            }
        }
        ins("@alice", "The demo build is ready, testers welcome!");
        // A second "alice" from another server — the chat shows the full
        // mxid for both, so the two don't get confused.
        ins("@alice:matrix.org", "Hello from matrix.org!");
        ins("@heidi", "Just joined! Hi everyone");
        ins("@bob", "Welcome @heidi! Check the pinned message.");
        ins("@ivan", "Anyone tried the new ui? https://github.com/progressive-chat/progressive-cli");
        ins("@you", "The ascii client looks great on my phone.");
        ins("@julia", "Screenshots or it didn't happen");
        ins("@kate", "Here you go: https://matrix.org/docs/guide");
        ins("@charlie", "E2EE finally works between devices.");
        ins("@dave", "Latency is way down after the sync rewrite.");
        ins("@erin", "Morning all! Coffee's on me.");
        ins("@frank", "Did the CI pass today?");
        ins("@grace", "Green across the board");
        ins("@carol", "The new poll feature is slick.");
        // A second poll in #general, voted on by the newcomers.
        {
            std::string pollId = "$demo_" + std::to_string(gt);
            matrix::Event p;
            p.event_id = pollId;
            p.room_id = "!general:demo.local"; p.sender = "@alice";
            p.type = "m.room.message";
            p.content = {{"msgtype", "m.poll.start"},
                         {"question", {{"text", "Meeting time tomorrow?"}}},
                         {"answers", {{{"id", "a"}, {"text", "Morning"}},
                                      {{"id", "b"}, {"text", "Evening"}}}},
                         {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}}};
            p.origin_server_ts = gt;
            dbi.insertEvent(p);
            gt -= 60000;
            struct { const char* sender; const char* vote; } votes2[] = {
                {"@heidi", "a"}, {"@ivan", "b"}, {"@julia", "a"},
                {"@kate", "b"}, {"@you", "a"}, {"@charlie", "b"},
            };
            for (auto& v : votes2) {
                matrix::Event r;
                r.event_id = "$demo_" + std::to_string(gt);
                r.room_id = "!general:demo.local"; r.sender = v.sender;
                r.type = "m.room.message";
                r.content = {{"msgtype", "m.poll.response"},
                             {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}},
                             {"selections", {v.vote}}};
                r.origin_server_ts = gt;
                dbi.insertEvent(r);
                gt -= 60000;
            }
        }
        ins("@ivan", "Voted! Morning works for me.");
        ins("@julia", "Evening is better for the EU folks.");
        ins("@heidi", "@bob where are the release notes?");
        ins("@bob", "Here: https://github.com/progressive-chat/progressive-cli/releases");
        ins("@kate", "Awesome work, the demo is impressive.");
        ins("@alice", "Meeting in 30, don't be late!");
        ins("@you", "On my way.");
        ins("@charlie", "Also relevant: https://spec.matrix.org/v1.13/");
        // A file share.
        {
            matrix::Event f;
            f.event_id = "$demo_" + std::to_string(gt);
            f.room_id = "!general:demo.local"; f.sender = "@dave";
            f.type = "m.room.message";
            f.content = {{"msgtype", "m.file"}, {"body", "roadmap-q3.pdf"},
                         {"url", "mxc://demo.local/roadmap_q3"},
                         {"filename", "roadmap-q3.pdf"}, {"mimetype", "application/pdf"}};
            f.origin_server_ts = gt;
            dbi.insertEvent(f);
            gt -= 120000;
        }
        ins("@erin", "Thanks @dave, looks solid.");
        ins("@heidi", "Count me in for tomorrow!");
        // Reactions on some of these messages.
        {
            auto reactTo = [&](const std::string& bodyPrefix, const char* key, const char* who) {
                auto evs = dbi.getEvents("!general:demo.local", 500);
                for (const auto& ev : evs) {
                    if (ev.content.value("body", "").find(bodyPrefix) != std::string::npos) {
                        matrix::Event r;
                        r.event_id = "$demo_" + std::to_string(gt);
                        r.room_id = "!general:demo.local"; r.sender = who;
                        r.type = "m.reaction";
                        r.content = {{"m.relates_to",
                                      {{"event_id", ev.event_id},
                                       {"rel_type", "m.annotation"},
                                       {"key", key}}}};
                        r.origin_server_ts = gt;
                        dbi.insertEvent(r);
                        gt -= 60000;
                        break;
                    }
                }
            };
            reactTo("The demo build is ready", "🤗", "@heidi");
            reactTo("The demo build is ready", "👍", "@ivan");
            reactTo("Here you go", "👍", "@bob");
            reactTo("Latency is way down", "🚀", "@alice");
            reactTo("Green across the board", "✅", "@you");
        }
        // The conversation keeps going — more unique messages.
        ins("@alice", "Roadmap review in #dev later, everyone is welcome.");
        ins("@you", "@alice adding it to my calendar now.");
        ins("@bob", "The new poll widget renders great on narrow screens.");
        ins("@julia", "Can someone test the invite flow? https://matrix.org/docs/guides/creating-rooms");
        ins("@kate", "On it, inviting @ivan to a fresh room now.");
        ins("@heidi", "This is the friendliest community I've joined in ages.");
        ins("@frank", "We should write this down for the docs.");
        ins("@charlie", "Noted, I will open an issue tonight.");
        ins("@dave", "Also: the sync loop got 5x faster after the rework.");
        ins("@erin", "My battery thanks you, @dave.");
        ins("@grace", "Who wants stickers for the community pack?");
        ins("@ivan", "Sticker pack? Count me in, @grace!");
        ins("@carol", "I can draw a few, pixel art style.");
        ins("@julia", "The sunset.png in #design is gorgeous, btw.");
        ins("@bob", "Link it in #general: https://matrix.to/#/!design:demo.local");
        ins("@you", "Done — pinned it for newcomers.");
        ins("@kate", "The ascii ui keeps impressing me. Well done, team.");
        ins("@alice", "v0.6 planning starts next week, ideas welcome.");
        ins("@heidi", "I have one: offline drafts!");
        ins("@frank", "Seconded, @heidi.");
        // More reactions on the conversation.
        {
            auto reactTo = [&](const std::string& bodyPrefix, const char* key, const char* who) {
                auto evs = dbi.getEvents("!general:demo.local", 500);
                for (const auto& ev : evs) {
                    if (ev.content.value("body", "").find(bodyPrefix) != std::string::npos) {
                        matrix::Event r;
                        r.event_id = "$demo_" + std::to_string(gt);
                        r.room_id = "!general:demo.local"; r.sender = who;
                        r.type = "m.reaction";
                        r.content = {{"m.relates_to",
                                      {{"event_id", ev.event_id},
                                       {"rel_type", "m.annotation"},
                                       {"key", key}}}};
                        r.origin_server_ts = gt;
                        dbi.insertEvent(r);
                        gt -= 60000;
                        break;
                    }
                }
            };
            reactTo("This is the friendliest", "\xf0\x9f\x92\x96", "@kate");
            reactTo("Who wants stickers", "\xf0\x9f\x91\x8d", "@you");
            reactTo("The ascii ui keeps impressing", "\xf0\x9f\x8f\x86", "@grace");
            reactTo("offline drafts", "\xf0\x9f\x92\xa1", "@alice");
            reactTo("My battery thanks you", "\xf0\x9f\x94\x8b", "@dave");
        }
        // A reply into the conversation (to heidi's last message).
        {
            std::string targetId;
            auto evs = dbi.getEvents("!general:demo.local", 500);
            for (const auto& ev : evs) {
                if (ev.content.value("body", "").find("Count me in") != std::string::npos) {
                    targetId = ev.event_id;
                    break;
                }
            }
            matrix::Event rep;
            rep.event_id = "$demo_" + std::to_string(gt);
            rep.room_id = "!general:demo.local"; rep.sender = "@kate";
            rep.type = "m.room.message";
            rep.content = {{"msgtype", "m.text"},
                           {"body", "> <@heidi:demo.local> Count me in for tomorrow!\nSee you there then!"},
                           {"m.relates_to",
                            {{"m.in_reply_to", {{"event_id", targetId}}}}}};
            rep.origin_server_ts = gt;
            dbi.insertEvent(rep);
        }
    }

    // The vote showcase: 13 more polls across 9 rooms (with the two #general
    // ones that makes 15 votings in 10 rooms) — what `demo vote` walks.
    // Placed before the fresh-message window so the room list previews stay
    // on the ordinary messages; each poll gets a few responses.
    {
        int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t vt = t0 - 95 * 60000;
        struct { const char* room; const char* question;
                 const char* a; const char* b; const char* c;
                 const char* voters; } polls[] = {
            {"!general:demo.local", "Which feature should ship first?", "Threads", "Reactions", "Spaces", "@bob/@charlie/@dave/@you"},
            {"!dev:demo.local", "Ship v0.6 this week?", "Yes, ship it", "One more beta", "Delay it", "@alice/@frank/@you"},
            {"!dev:demo.local", "Which linter do we standardize on?", "clang-tidy", "clang-format", "None", "@dave/@carol"},
            {"!random:demo.local", "Weekly movie night?", "Friday", "Saturday", "Skip this week", "@erin/@heidi/@you"},
            {"!random:demo.local", "Next community event?", "Hackathon", "Game night", "Movie night", "@frank/@grace"},
            {"!design:demo.local", "Dark or light theme for the app?", "Dark", "Light", "System default", "@alice/@julia"},
            {"!music:demo.local", "Genre of the month?", "Synthwave", "Jazz", "Metal", "@erin/@bob/@you"},
            {"!games:demo.local", "Community tournament game?", "Chess", "Mario Kart", "Fighting game", "@kate/@ivan/@you"},
            {"!games:demo.local", "Controller or keyboard?", "Controller", "Keyboard", "Touch", "@grace/@dave"},
            {"!science:demo.local", "Host a science AMA?", "Yes, monthly", "Once", "No", "@carol/@bob/@you"},
            {"!movies:demo.local", "Best sci-fi movie of the year?", "Dune Part 3", "The Martian remake", "Other", "@julia/@charlie"},
            {"!food:demo.local", "Meetup catering?", "Pizza", "Vegan bowls", "BBQ", "@heidi/@erin/@you"},
            {"!privacy:demo.local", "Password manager?", "Bitwarden", "KeepassXC", "pass", "@frank/@kate/@you"},
        };
        int n = 0;
        for (const auto& pl : polls) {
            std::string pollId = "$demo_vote_" + std::to_string(++n);
            matrix::Event p;
            p.event_id = pollId;
            p.room_id = pl.room; p.sender = "@bob";
            p.type = "m.room.message";
            p.content = {{"msgtype", "m.poll.start"},
                         {"question", {{"text", pl.question}}},
                         {"answers", {{{"id", "a"}, {"text", pl.a}},
                                      {{"id", "b"}, {"text", pl.b}},
                                      {{"id", "c"}, {"text", pl.c}}}},
                         {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}}};
            p.origin_server_ts = vt;
            dbi.insertEvent(p);
            vt -= 60000;
            std::string voters = pl.voters;
            size_t start = 0;
            size_t vi = 0;
            while (start <= voters.size()) {
                size_t slash = voters.find('/', start);
                std::string who = voters.substr(
                    start, slash == std::string::npos ? std::string::npos : slash - start);
                if (!who.empty()) {
                    // The answers rotate, so the tallies are mixed.
                    const char* vote = who == "@you"
                        ? (vi % 2 == 0 ? "a" : "b")
                        : (vi % 3 == 0 ? "a" : vi % 3 == 1 ? "b" : "c");
                    matrix::Event r;
                    r.event_id = "$demo_vote_r_" + std::to_string(vi) + "_" + who;
                    r.room_id = pl.room; r.sender = who;
                    r.type = "m.room.message";
                    r.content = {{"msgtype", "m.poll.response"},
                                 {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}},
                                 {"selections", {vote}}};
                    r.origin_server_ts = vt;
                    dbi.insertEvent(r);
                    vi++;
                    vt -= 60000;
                }
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
        }
    }
    // The notifications corner: #design is monitored at 100% and gets read
    // receipts, so the bottom-right corner shows both kinds (receipts +
    // the @you pings that live in the room).
    dbi.setSetting("monitor:!design:demo.local", "100");
    {
        int64_t n0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        struct { const char* who; int64_t ago; } reads[] = {
            {"@carol:demo.local", 12 * 60000},
            {"@kate:demo.local", 5 * 60000},
        };
        for (const auto& rd : reads) {
            int64_t rts = n0 - rd.ago;
            matrix::Event rc;
            rc.event_id = "$demo_receipt_" + std::string(rd.who);
            rc.room_id = "!design:demo.local";
            rc.sender = rd.who;
            rc.type = "m.receipt";
            rc.content = {{"$demo_design_msg",
                           {{"m.read", {{rd.who, {{"ts", rts}}}}}}}};
            rc.origin_server_ts = rts;
            dbi.insertEvent(rc);
        }
        // A message of ours + a reply to it + a receipt on it: the corner
        // shows "replied to you" and "saw your message" for real.
        matrix::Event mine;
        mine.event_id = "$demo_you_pair_msg";
        mine.room_id = "!design:demo.local";
        mine.sender = "@you";
        mine.type = "m.room.message";
        mine.content = {{"body",
                         "Who wants to pair on the sync rework? "
                         "I'll take the first half."},
                        {"msgtype", "m.text"}};
        mine.origin_server_ts = n0 - 22 * 60000;
        dbi.insertEvent(mine);
        matrix::Event reply;
        reply.event_id = "$demo_reply_pair_msg";
        reply.room_id = "!design:demo.local";
        reply.sender = "@bob:demo.local";
        reply.type = "m.room.message";
        reply.content = {{"body", "I'll take the second half!"},
                         {"msgtype", "m.text"},
                         {"m.relates_to",
                          {{"m.in_reply_to", {{"event_id", mine.event_id}}}}}};
        reply.origin_server_ts = n0 - 18 * 60000;
        dbi.insertEvent(reply);
        matrix::Event seen;
        seen.event_id = "$demo_receipt_you_pair";
        seen.room_id = "!design:demo.local";
        seen.sender = "@kate:demo.local";
        seen.type = "m.receipt";
        seen.content = {{mine.event_id,
                         {{"m.read",
                           {{"@kate:demo.local", {{"ts", n0 - 8 * 60000}}}}}}}};
        seen.origin_server_ts = n0 - 8 * 60000;
        dbi.insertEvent(seen);
    }
    // The room-upgrade showcase: #dev got a tombstone and moved to
    // #programming — the chat shows the upgrade banner + "goto successor".
    {
        int64_t t2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        matrix::Event bye;
        bye.event_id = "$demo_dev_goodbye";
        bye.room_id = "!dev:demo.local";
        bye.sender = "@alice:demo.local";
        bye.type = "m.room.message";
        bye.content = {{"body",
                        "We moved the dev chat to #programming \xe2\x80\x94 "
                        "join us there!"},
                       {"msgtype", "m.text"}};
        bye.origin_server_ts = t2 - 26 * 3600000;
        dbi.insertEvent(bye);
        matrix::Event tomb;
        tomb.event_id = "$demo_dev_tombstone";
        tomb.room_id = "!dev:demo.local";
        tomb.sender = "@alice:demo.local";
        tomb.type = "m.room.tombstone";
        tomb.state_key = "";
        tomb.content = {{"body", "This room has been replaced"},
                        {"successor_room_id", "!programming:demo.local"}};
        tomb.origin_server_ts = t2 - 25 * 3600000;
        dbi.insertEvent(tomb);
    }
}
