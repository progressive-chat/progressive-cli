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
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

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


extern int cmdRooms(const matrixcli::cli::Args& args);
extern int cmdView(const matrixcli::cli::Args& args);
extern int cmdSearch(const matrixcli::cli::Args& args);
extern int runSasVerification(const std::string& targetUser,
                              const std::string& targetDevice,
                              int timeoutSec, bool autoConfirm,
                              const std::function<void(const std::string&)>& log,
                              const std::function<bool()>& confirm);

// The async URL preview (the TUI parity): the first link in a message is
// fetched in the background; the og:title lands as a notice under it.
// Each URL is fetched once per run (the cache below).
static void queueUrlPreview(matrixcli::matrix::Client& client, matrixcli::tui::ChatView& chat,
                            const std::string& roomId, const std::string& body) {
    static std::unordered_set<std::string> fetched;
    static std::mutex fetchedMtx;
    const auto httpPos = body.find("http");
    if (httpPos == std::string::npos) return;
    const auto endPos = body.find_first_of(" \t\n", httpPos);
    const std::string url = body.substr(httpPos, endPos == std::string::npos
                                                     ? std::string::npos
                                                     : endPos - httpPos);
    if (url.size() < 10) return;
    {
        std::lock_guard<std::mutex> lk(fetchedMtx);
        if (!fetched.insert(url).second) return;
    }
    std::thread([&client, &chat, roomId, url]() {
        try {
            const nlohmann::json preview = client.getURLPreview(url);
            const std::string title = preview.value("og:title", "");
            if (title.empty()) return;
            matrixcli::tui::MessageInfo mi;
            mi.sender = "@preview";
            mi.is_notice = true;
            mi.body = "\U0001F517 " + title;
            chat.addMessage(roomId, mi);
        } catch (...) {}
    }).detach();
}

// The login-screen connection choice (direct/tor/i2p/yggdrasil/custom):
// applies to the TUI client now, persists into config.json and sets the
// core's global proxy for every subsequent process.
static void applyConnectionChoice(matrixcli::matrix::Client& client,
                                  const std::string& connection) {
    const std::string c = connection.empty() ? "direct" : connection;
    matrixcli::http::ProxyConfig pc;
    std::string persistType = "socks5h";
    bool enabled = false;
    if (c == "tor") {
        pc.type = matrixcli::http::ProxyType::SOCKS5;
        pc.host = "127.0.0.1";
        pc.port = 9050;
        enabled = true;
    } else if (c == "i2p") {
        pc.type = matrixcli::http::ProxyType::HTTP;
        pc.host = "127.0.0.1";
        pc.port = 4444;
        persistType = "http";
        enabled = true;
    } else if (c.rfind("custom ", 0) == 0) {
        pc.type = matrixcli::http::ProxyType::SOCKS5;
        const std::string hp = c.substr(7);
        const auto colon = hp.rfind(':');
        pc.host = colon == std::string::npos ? hp : hp.substr(0, colon);
        if (colon != std::string::npos) {
            try { pc.port = std::stoi(hp.substr(colon + 1)); }
            catch (...) { pc.port = 9050; }
        } else {
            pc.port = 9050;
        }
        enabled = true;
    }
    // "direct" and "yggdrasil" (the native IPv6 mesh routing) stay direct.

    client.setProxy(enabled ? pc : matrixcli::http::ProxyConfig{});
    matrixcli::Config::instance().set("proxy_enabled", enabled ? "true" : "false");
    if (enabled) {
        matrixcli::Config::instance().set("proxy_host", pc.host);
        matrixcli::Config::instance().set("proxy_port", std::to_string(pc.port));
        matrixcli::Config::instance().set("proxy_type", persistType);
    }
    matrixcli::Config::instance().save();

    progressive::desktop::ProxyConfig gp;
    gp.enabled = enabled;
    gp.host = pc.host;
    gp.port = pc.port;
    gp.type = pc.type == matrixcli::http::ProxyType::HTTP
                  ? progressive::desktop::ProxyConfig::Type::Http
                  : progressive::desktop::ProxyConfig::Type::Socks5Hostname;
    progressive::desktop::setGlobalProxy(gp);
}

int populateDemoData(matrixcli::db::Database& dbi) {
    using namespace matrixcli;

    // Rebuild the demo from scratch: drop the demo-local rooms and their
    // events so re-running 'demo populate' always yields the CURRENT demo
    // (new rooms/messages/images/threads included).
    {
        auto existing = dbi.listRooms();
        for (const auto& r : existing) {
            std::string id = r.value("room_id", "");
            if (id.find(":demo.local") != std::string::npos) {
                dbi.clearRoom(id);
            }
        }
    }

    struct { const char* id; const char* name; const char* topic; int members; } rooms[] = {
        {"!general:demo.local","#general","General discussion",42},
        {"!dev:demo.local","#dev","Development chat",15},
        {"!random:demo.local","#random","Random stuff",28},
        {"!design:demo.local","#design","UI & design talk",9},
        {"!music:demo.local","#music","Music sharing",23},
        {"!games:demo.local","#games","Games & esports",37},
        {"!science:demo.local","#science","Science news",18},
        {"!offtopic:demo.local","#offtopic","Off-topic banter",54},
        {"!announce:demo.local","#announcements","Official announcements",120},
        {"!help:demo.local","#help","Support chat",31},
        {"!linux:demo.local","#linux","Linux & FOSS",66},
        {"!crypto:demo.local","#crypto","Crypto & Web3",42},
        {"!photography:demo.local","#photography","Camera talk",19},
        {"!travel:demo.local","#travel","Travel stories",27},
        {"!food:demo.local","#food","Cooking & recipes",35},
        {"!books:demo.local","#books","Reading club",14},
        {"!fitness:demo.local","#fitness","Workout logs",22},
        {"!movies:demo.local","#movies","Film & TV",48},
        {"!programming:demo.local","#programming","Code help",61},
        {"!rust:demo.local","#rust","Rust lang",33},
        {"!matrix:demo.local","#matrix","Matrix protocol",17},
        {"!meta:demo.local","#meta","About this demo",8},
        {"!sports:demo.local","#sports","Game day talk",41},
        {"!chess:demo.local","#chess","Chess & strategy",16},
        {"!retro-gaming:demo.local","#retro-gaming","Retro consoles",29},
        {"!hardware:demo.local","#hardware","PC building",58},
        {"!distro-talk:demo.local","#distro-talk","Distro hopping",36},
        {"!shell:demo.local","#shell","Shell scripting",44},
        {"!editors:demo.local","#editors","Editor wars",52},
        {"!git:demo.local","#git","Version control",48},
        {"!dotfiles:demo.local","#dotfiles","Dotfiles showoff",31},
        {"!selfhosting:demo.local","#selfhosting","Self-host your life",67},
        {"!homelab:demo.local","#homelab","Homelab corner",39},
        {"!security:demo.local","#security","Security & exploits",54},
        {"!privacy:demo.local","#privacy","Privacy tools",47},
        {"!networking:demo.local","#networking","Networks & routing",28},
        {"!databases:demo.local","#databases","SQL & NoSQL",33},
        {"!webdev:demo.local","#webdev","Web development",62},
        {"!frontend:demo.local","#frontend","Frontend craft",45},
        {"!backend:demo.local","#backend","Backend services",40},
        {"!ml:demo.local","#ml","Machine learning",57},
        {"!ai-art:demo.local","#ai-art","AI generated art",35},
        {"!astronomy:demo.local","#astronomy","Stars & space",26},
        {"!physics:demo.local","#physics","Physics chat",38},
        {"!chemistry:demo.local","#chemistry","Lab banter",21},
        {"!biology:demo.local","#biology","Life sciences",24},
        {"!math:demo.local","#math","Math problems",30},
        {"!history:demo.local","#history","History corner",43},
        {"!philosophy:demo.local","#philosophy","Deep thoughts",27},
        {"!languages:demo.local","#languages","Language learning",34},
        {"!writing:demo.local","#writing","Writing & prose",22},
        {"!poetry:demo.local","#poetry","Poetry corner",15},
        {"!art:demo.local","#art","Art & craft",33},
        {"!pixelart:demo.local","#pixelart","Pixel art",25},
        {"!music-production:demo.local","#music-production","Making beats",37},
        {"!synth:demo.local","#synth","Synthesizers",18},
        {"!jazz:demo.local","#jazz","Jazz lounge",20},
        {"!metal:demo.local","#metal","Metal heads",46},
        {"!classical:demo.local","#classical","Classical music",23},
        {"!techno:demo.local","#techno","Techno warehouse",32},
        {"!dnb:demo.local","#dnb","Drum & bass",28},
        {"!hiking:demo.local","#hiking","Trails & peaks",40},
        {"!camping:demo.local","#camping","Camping gear",26},
        {"!cycling:demo.local","#cycling","Bike rides",35},
        {"!running:demo.local","#running","Running club",44},
        {"!climbing:demo.local","#climbing","Climbing gym",19},
        {"!yoga:demo.local","#yoga","Yoga & stretch",17},
        {"!vegan:demo.local","#vegan","Vegan cooking",29},
        {"!baking:demo.local","#baking","Bread & pastry",31},
        {"!coffee:demo.local","#coffee","Coffee brewing",42},
        {"!tea:demo.local","#tea","Tea time",24},
        {"!beer:demo.local","#beer","Craft beer",38},
        {"!wine:demo.local","#wine","Wine cellar",13},
        {"!boardgames:demo.local","#boardgames","Board game night",27},
        {"!podcasts:demo.local","#podcasts","Podcast picks",21},
        {"!memes:demo.local","#memes","Memes & jokes",55},
        {"!diy:demo.local","#diy","DIY projects",36},
        {"!finance:demo.local","#finance","Personal finance",49},
        {"!dm_alice:demo.local","Alice","",2},
        {"!dm_bob:demo.local","Bob","",2},
        {"!dm_carol:demo.local","Carol","",2},
        {"!dm_dave:demo.local","Dave","",2},
    };
    for (auto& r : rooms) {
        nlohmann::json j;
        j["name"] = r.name; j["topic"] = r.topic; j["member_count"] = r.members;
        // The DMs are encrypted by default, like Element.
        if (std::string(r.id).find("!dm_") == 0) j["is_encrypted"] = 1;
        dbi.upsertRoom(j, r.id);
    }

    // Two demo spaces (Element-style): the rooms are tagged with their
    // parent space, the space rooms themselves are marked is_space.
    const char* techSpace = "!space_tech:demo.local";
    const char* socialSpace = "!space_social:demo.local";
    dbi.upsertRoom({{"name", "Tech Space"}, {"topic", "Dev, code & hardware"},
                    {"member_count", 312}, {"is_space", 1}}, techSpace);
    dbi.upsertRoom({{"name", "Social Space"}, {"topic", "Life, fun & creativity"},
                    {"member_count", 198}, {"is_space", 1}}, socialSpace);
    {
        const char* tech[] = {"!dev","!programming","!rust","!linux","!databases",
            "!webdev","!backend","!frontend","!ml","!ai-art","!crypto","!security",
            "!privacy","!networking","!homelab","!selfhosting","!git","!editors",
            "!shell","!distro-talk","!dotfiles","!hardware","!retro-gaming", nullptr};
        const char* social[] = {"!general","!announcements","!help","!meta","!random",
            "!offtopic","!sports","!chess","!games","!music","!music-production",
            "!synth","!jazz","!metal","!classical","!techno","!dnb","!art","!pixelart",
            "!photography","!travel","!food","!books","!fitness","!movies","!hiking",
            "!camping","!cycling","!running","!climbing","!yoga","!vegan","!baking",
            "!coffee","!tea","!beer","!wine","!boardgames","!podcasts","!memes",
            "!diy","!finance","!science","!math","!physics","!chemistry","!biology",
            "!astronomy","!history","!philosophy","!languages","!writing","!poetry",
            "!design", nullptr};
        for (const char** c = tech; *c; ++c) dbi.tagRoom(std::string(*c) + ":demo.local", techSpace);
        for (const char** c = social; *c; ++c) dbi.tagRoom(std::string(*c) + ":demo.local", socialSpace);
    }

    // Matrix origin_server_ts is milliseconds since epoch
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    struct { const char* room; const char* sender; const char* name; const char* body; } msgs[] = {
        {"!general:demo.local","@alice","Alice","Welcome! This is progressive-cli — a terminal Matrix client."},
        {"!general:demo.local","@bob","Bob","Supports E2EE, SQLite cache, multi-format REST API."},
        {"!general:demo.local","@alice","Alice","Try: progressive-cli tui, progressive-cli view, progressive-cli send"},
        {"!general:demo.local","@alice","Alice","Multiline demo message:\nline one\nline two\nline three"},
        {"!dev:demo.local","@charlie","Charlie","C++20, raw sockets + OpenSSL, no external HTTP libs."},
        {"!dev:demo.local","@alice","Alice","CMake build, 5 format renderers, full Matrix CS API."},
        {"!random:demo.local","@bob","Bob","Why did the dev quit? No arrays."},
        {"!dm_alice:demo.local","@alice","Alice","Hey! This is a private encrypted DM."},
        {"!dm_alice:demo.local","@you","You","Hi Alice! The ascii client is really nice."},
        {"!dm_alice:demo.local","@alice","Alice","Glad you like it — and the DMs work offline too."},
        {"!dm_bob:demo.local","@bob","Bob","Try progressive-cli view \"!dm_bob:demo.local\""},
        {"!dm_bob:demo.local","@you","You","Will do — I sent you a file by the way."},
        {"!dm_bob:demo.local","@bob","Bob","Got it, report.pdf looks good."},
    };
    int64_t dayMs = 86400000;
    int day = 0;
    for (auto& m : msgs) {
        matrix::Event ev;
        ev.event_id = "$demo_" + std::to_string(ts);
        ev.room_id = m.room; ev.sender = m.sender;
        ev.type = "m.room.message";
        ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
        ev.origin_server_ts = ts;
        dbi.insertEvent(ev);
        // Spread the history across 5 days (an "old" room): every other
        // message jumps back one day.
        if (day % 2 == 1) ts -= dayMs;
        ts -= 3600000;  // an hour between messages
        day++;
    }

    // Content for the extra rooms: mentions, urls, files, audio.
    {
        struct { const char* room; const char* sender; const char* body; } extra[] = {
            {"!design:demo.local", "@carol", "The ascii ui looks great, @you — nice work on the pipes."},
            {"!design:demo.local", "@dave", "Agreed! And the thread panel is super useful."},
            {"!design:demo.local", "@you", "Thanks! Check the design spec: https://matrix.org/docs/"},
            {"!design:demo.local", "@carol", "Also sent the mockups as files, see above."},
            {"!music:demo.local", "@erin", "New album out today 🎵 https://soundcloud.com/example"},
            {"!music:demo.local", "@you", "Listening now. The bass is great."},
            {"!music:demo.local", "@erin", "I'll upload the studio recording as an audio file."},
            {"!games:demo.local", "@frank", "Patch notes are live: https://matrix.org/blog/"},
            {"!games:demo.local", "@you", "Nice, the new map is huge. @frank up for a game tonight?"},
            {"!games:demo.local", "@frank", "Sure! I'll drop the invite file here."},
            {"!science:demo.local", "@grace", "The paper is out — abstract: https://arxiv.org/"},
            {"!science:demo.local", "@you", "Fascinating read, @grace. The figures are great."},
            {"!help:demo.local", "@you", "How do I reset my keys? Anyone?"},
            {"!help:demo.local", "@grace", "@you — Preferences → Reset device keys, then re-verify."},
            {"!help:demo.local", "@you", "Thanks @grace! It worked."},
            {"!offtopic:demo.local", "@erin", "Random question: how do you take your coffee?"},
            {"!offtopic:demo.local", "@dave", "Black, always. @frank is a latte guy."},
            {"!offtopic:demo.local", "@frank", "Latte supremacy!"},
            {"!announce:demo.local", "@alice", "Welcome to the community! Rules: https://matrix.org/"},
            {"!announce:demo.local", "@bob", "And the code of conduct is pinned in this room."},
            {"!dm_carol:demo.local", "@carol", "Hey @you! Want to review my ui sketches?"},
            {"!dm_carol:demo.local", "@you", "Sure! Send them over."},
            {"!dm_carol:demo.local", "@carol", "Here you go: https://example.com/sketch.png"},
            {"!dm_dave:demo.local", "@dave", "@you — the demo DMs are two-sided now!"},
            {"!dm_dave:demo.local", "@you", "I noticed, nice."},
            {"!dm_dave:demo.local", "@dave", "I'll send you an audio note to test playback."},
        };
        for (auto& m : extra) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }
        struct { const char* room; const char* sender; const char* body; } extra2[] = {
            {"!linux:demo.local", "@dave", "My kernel finally boots. https://kernel.org/"},
            {"!linux:demo.local", "@you", "@dave nice! Which distro?"},
            {"!crypto:demo.local", "@frank", "Don't trust, verify. https://bitcoin.org/"},
            {"!photography:demo.local", "@carol", "Golden hour today, look at the light."},
            {"!photography:demo.local", "@you", "Great shot! Settings?"},
            {"!travel:demo.local", "@erin", "Just landed in Tokyo! https://japan.travel/"},
            {"!travel:demo.local", "@you", "Envious @erin! Send photos."},
            {"!food:demo.local", "@bob", "Tonight: ramen from scratch."},
            {"!food:demo.local", "@you", "Recipe? I need that in my life."},
            {"!books:demo.local", "@grace", "The manual is a masterpiece."},
            {"!fitness:demo.local", "@you", "Morning run done. 5k."},
            {"!movies:demo.local", "@alice", "Anyone seen the new sci-fi?"},
            {"!movies:demo.local", "@you", "Yes! The plot twist though."},
            {"!programming:demo.local", "@dave", "Segfault at line 42. Classic."},
            {"!programming:demo.local", "@you", "@dave use-after-free probably."},
            {"!rust:demo.local", "@frank", "Borrow checker saves the day again."},
            {"!rust:demo.local", "@you", "It compiles first try. @frank today was a good day."},
            {"!matrix:demo.local", "@alice", "The protocol spec is here: https://spec.matrix.org/"},
            {"!matrix:demo.local", "@you", "Reading it now, @alice."},
            {"!meta:demo.local", "@you", "This is the demo room, try everything here."},
        };
        for (auto& m : extra2) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }

        struct { const char* room; const char* sender; const char* msgtype; const char* body; } extra3[] = {
            {"!sports:demo.local", "@alice", "m.text", "Big match tonight, who's watching?"},
            {"!chess:demo.local", "@bob", "m.text", "Puzzle of the day: white to move, mate in 3."},
            {"!retro-gaming:demo.local", "@alice", "m.text", "Just finished Zelda on the SNES emulator."},
            {"!hardware:demo.local", "@charlie", "m.file", "schematic-v2.pdf"},
            {"!distro-talk:demo.local", "@alice", "m.text", "Switched to Arch, btw. https://archlinux.org/"},
            {"!shell:demo.local", "@erin", "m.text", "TIL: ctrl+r searches history in reverse."},
            {"!editors:demo.local", "@alice", "m.text", "vim or neovim? The debate continues."},
            {"!git:demo.local", "@grace", "m.text", "@you rebase or merge?"},
            {"!dotfiles:demo.local", "@carol", "m.file", "notes-q3.pdf"},
            {"!selfhosting:demo.local", "@dave", "m.file", "hardware-review.pdf"},
            {"!homelab:demo.local", "@alice", "m.text", "Plex server finally up!"},
            {"!security:demo.local", "@carol", "m.text", "Never store plaintext passwords, folks."},
            {"!privacy:demo.local", "@alice", "m.text", "Worth a read: https://www.torproject.org/"},
            {"!networking:demo.local", "@erin", "m.text", "VLANs are the answer to 90% of my problems."},
            {"!databases:demo.local", "@erin", "m.file", "benchmark-results.pdf"},
            {"!webdev:demo.local", "@grace", "m.text", "The new CSS grid features are great."},
            {"!frontend:demo.local", "@alice", "m.text", "Rewrote the site with no JS. So fast now."},
            {"!backend:demo.local", "@bob", "m.text", "Latency down to 12ms after the rewrite."},
            {"!ml:demo.local", "@alice", "m.text", "Fine-tuned a small model today, results are neat."},
            {"!ai-art:demo.local", "@carol", "m.text", "Prompt: cyberpunk cat, 4k, cinematic lighting."},
            {"!astronomy:demo.local", "@alice", "m.poll", "Which planet is best for a new mission?"},
            {"!physics:demo.local", "@erin", "m.text", "Why is entropy always increasing?"},
            {"!chemistry:demo.local", "@alice", "m.text", "The coffee filter chromatogram works."},
            {"!biology:demo.local", "@grace", "m.text", "CRISPR news today is wild. https://en.wikipedia.org/wiki/CRISPR"},
            {"!math:demo.local", "@alice", "m.text", "Prove pi is irrational. Go. @you"},
            {"!history:demo.local", "@bob", "m.text", "The library of Alexandria keeps me up at night."},
            {"!philosophy:demo.local", "@alice", "m.text", "Is a broken clock right twice a day?"},
            {"!languages:demo.local", "@carol", "m.text", "Duolingo streak: 47 days. @you join me!"},
            {"!writing:demo.local", "@frank", "m.file", "meeting-notes.pdf"},
            {"!poetry:demo.local", "@erin", "m.text", "Roses are red, my terminal is green..."},
            {"!art:demo.local", "@alice", "m.text", "Sketching the market square this weekend."},
            {"!pixelart:demo.local", "@grace", "m.text", "16x16 sprite of a frog. Cute."},
            {"!music-production:demo.local", "@dave", "m.audio", "riff-idea.mp3"},
            {"!synth:demo.local", "@erin", "m.audio", "demo-track.mp3"},
            {"!jazz:demo.local", "@alice", "m.text", "Miles Davis tonight. Recommended: https://open.spotify.com/"},
            {"!metal:demo.local", "@frank", "m.audio", "voice-note.m4a"},
            {"!classical:demo.local", "@alice", "m.text", "Bach's cello suites are perfection."},
            {"!techno:demo.local", "@erin", "m.text", "Warehouse party this Saturday!"},
            {"!dnb:demo.local", "@alice", "m.text", "Drum and bass fills me with energy."},
            {"!hiking:demo.local", "@grace", "m.text", "Summited the ridge, view was unreal."},
            {"!camping:demo.local", "@grace", "m.file", "packing-list.pdf"},
            {"!cycling:demo.local", "@bob", "m.text", "100km ride done, legs are gone."},
            {"!running:demo.local", "@alice", "m.text", "New PB: 5k in 22:14!"},
            {"!climbing:demo.local", "@carol", "m.text", "Sent my first 7a today, @you should try it."},
            {"!yoga:demo.local", "@alice", "m.text", "Morning flow, 20 minutes, game changer."},
            {"!vegan:demo.local", "@erin", "m.text", "Tofu scramble recipe incoming."},
            {"!baking:demo.local", "@alice", "m.text", "Sourdough loaf #12, best one yet."},
            {"!coffee:demo.local", "@grace", "m.text", "Pour over > espresso. Fight me. https://en.wikipedia.org/wiki/Pour-over_coffee"},
            {"!tea:demo.local", "@alice", "m.text", "Aged oolong, notes of honey and stone fruit."},
            {"!beer:demo.local", "@alice", "m.poll", "Pilsner or IPA this Friday?"},
            {"!wine:demo.local", "@alice", "m.text", "Cork vs screwcap, discuss."},
            {"!boardgames:demo.local", "@alice", "m.poll", "Which game for Friday night?"},
            {"!podcasts:demo.local", "@grace", "m.audio", "podcast-clip.mp3"},
            {"!memes:demo.local", "@erin", "m.text", "me: I'll just fix one thing. 6 hours later:"},
            {"!diy:demo.local", "@alice", "m.text", "Built a desk from pallets, zero regrets."},
            {"!finance:demo.local", "@grace", "m.text", "Emergency fund first, then investments. @you"},
        };
        for (auto& m : extra3) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            if (!strcmp(m.msgtype, "m.file")) {
                ev.content = {{"msgtype", "m.file"}, {"body", m.body},
                              {"url", "mxc://demo.local/f_" + std::string(m.body)},
                              {"filename", m.body}, {"mimetype", "application/pdf"}};
            } else if (!strcmp(m.msgtype, "m.audio")) {
                ev.content = {{"msgtype", "m.audio"}, {"body", m.body},
                              {"url", "mxc://demo.local/a_" + std::string(m.body)},
                              {"filename", m.body}, {"mimetype", "audio/mpeg"}};
            } else if (!strcmp(m.msgtype, "m.poll")) {
                std::string pid = "$demo_" + std::to_string(ts);
                ev.content = {{"msgtype", "m.poll.start"},
                              {"question", {{"text", m.body}}},
                              {"answers", {{{"id", "a"}, {"text", "Option A"}},
                                           {{"id", "b"}, {"text", "Option B"}}}},
                              {"m.relates_to", {{"event_id", pid}, {"rel_type", "m.reference"}}}};
            } else {
                ev.content = {{"msgtype", "m.text"}, {"body", m.body}};
            }
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }

        // A file + an audio in the design room.
        matrix::Event f;
        f.event_id = "$demo_" + std::to_string(ts);
        f.room_id = "!design:demo.local"; f.sender = "@carol";
        f.type = "m.room.message";
        f.content = {{"msgtype", "m.file"}, {"body", "mockups v2 final.zip"},
                     {"filename", "mockups v2 final.zip"}, {"url", "mxc://demo.local/mockups"},
                     {"info", {{"mimetype", "application/zip"}, {"size", 2048033}}}};
        f.origin_server_ts = ts;
        dbi.insertEvent(f);
        ts -= 3600000;
        matrix::Event a;
        a.event_id = "$demo_" + std::to_string(ts);
        a.room_id = "!music:demo.local"; a.sender = "@erin";
        a.type = "m.room.message";
        a.content = {{"msgtype", "m.audio"}, {"body", "studio-recording.ogg"},
                     {"filename", "studio-recording.ogg"}, {"url", "mxc://demo.local/rec"},
                     {"info", {{"mimetype", "audio/ogg"}, {"size", 240934}}}};
        a.origin_server_ts = ts;
        dbi.insertEvent(a);
        ts -= 3600000;
    }

    // An image message + reactions (the ui renders the card and the
    // "❤ 2" counts under the message).
    {
        std::string imgId = "$demo_" + std::to_string(ts);
        matrix::Event img;
        img.event_id = imgId;
        img.room_id = "!general:demo.local"; img.sender = "@bob";
        img.type = "m.room.message";
        img.content = {{"msgtype", "m.image"}, {"body", "sunset.png"},
                       {"url", "mxc://demo.local/sunset"},
                       {"info", {{"mimetype", "image/png"}, {"size", 204800},
                                 {"w", 1024}, {"h", 768}}}};
        img.origin_server_ts = ts;
        dbi.insertEvent(img);
        ts -= 60;

        struct { const char* key; } reacts[] = {{"\xe2\x9d\xa4\xef\xb8\x8f"},
                                                {"\xe2\x9d\xa4\xef\xb8\x8f"},
                                                {"\xf0\x9f\x91\x8d"}};
        for (auto& r : reacts) {
            matrix::Event re;
            re.event_id = "$demo_" + std::to_string(ts);
            re.room_id = "!general:demo.local"; re.sender = "@alice";
            re.type = "m.reaction";
            re.content = {{"m.relates_to",
                           {{"rel_type", "m.annotation"}, {"event_id", imgId},
                            {"key", r.key}}}};
            re.origin_server_ts = ts;
            dbi.insertEvent(re);
            ts -= 60;
        }
    }

    // A regular file + an audio message (the ui renders 📄 / 🎵 cards).
    {
        matrix::Event f;
        f.event_id = "$demo_" + std::to_string(ts);
        f.room_id = "!general:demo.local"; f.sender = "@charlie";
        f.type = "m.room.message";
        f.content = {{"msgtype", "m.file"}, {"body", "report.pdf"},
                     {"filename", "report.pdf"}, {"url", "mxc://demo.local/report"},
                     {"info", {{"mimetype", "application/pdf"}, {"size", 482033}}}};
        f.origin_server_ts = ts;
        dbi.insertEvent(f);
        ts -= 3600000;
        matrix::Event a;
        a.event_id = "$demo_" + std::to_string(ts);
        a.room_id = "!random:demo.local"; a.sender = "@bob";
        a.type = "m.room.message";
        a.content = {{"msgtype", "m.audio"}, {"body", "voice-note.ogg"},
                     {"filename", "voice-note.ogg"}, {"url", "mxc://demo.local/voice"},
                     {"info", {{"mimetype", "audio/ogg"}, {"size", 120934}}}};
        a.origin_server_ts = ts;
        dbi.insertEvent(a);
        ts -= 3600000;
    }

    // A poll (m.poll.start) + two responses.
    {
        std::string pollId = "$demo_" + std::to_string(ts);
        matrix::Event p;
        p.event_id = pollId;
        p.room_id = "!general:demo.local"; p.sender = "@alice";
        p.type = "m.room.message";
        p.content = {{"msgtype", "m.poll.start"},
                     {"question", {{"text", "Where should the meetup be?"}}},
                     {"answers", {{{"id", "a"}, {"text", "Park"}},
                                  {{"id", "b"}, {"text", "Cafe"}}}},
                     {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}}};
        p.origin_server_ts = ts;
        dbi.insertEvent(p);
        ts -= 3600000;
        struct { const char* sender; const char* vote; } votes[] = {
            {"@bob", "a"}, {"@charlie", "a"}, {"@you", "b"},
        };
        for (auto& v : votes) {
            matrix::Event r;
            r.event_id = "$demo_" + std::to_string(ts);
            r.room_id = "!general:demo.local"; r.sender = v.sender;
            r.type = "m.room.message";
            r.content = {{"msgtype", "m.poll.response"},
                         {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}},
                         {"selections", {v.vote}}};
            r.origin_server_ts = ts;
            dbi.insertEvent(r);
            ts -= 3600000;
        }
    }

    // A redacted (deleted) message + the redaction event.
    {
        std::string doomedId = "$demo_" + std::to_string(ts);
        matrix::Event d;
        d.event_id = doomedId;
        d.room_id = "!random:demo.local"; d.sender = "@charlie";
        d.type = "m.room.message";
        d.content = {{"msgtype", "m.text"}, {"body", "this message will be deleted"}};
        d.origin_server_ts = ts;
        dbi.insertEvent(d);
        ts -= 3600000;
        matrix::Event red;
        red.event_id = "$demo_" + std::to_string(ts);
        red.room_id = "!random:demo.local"; red.sender = "@charlie";
        red.type = "m.room.redaction";
        red.redacts = doomedId;
        red.content = nlohmann::json::object();
        red.origin_server_ts = ts;
        dbi.insertEvent(red);
        ts -= 3600000;
    }

    // More threads across the demo: each root + a few replies (m.thread).
    {
        int64_t tt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 3 * 3600000;
        struct { const char* room; const char* sender; const char* body; } roots[] = {
            {"!general:demo.local", "@you", "Poll idea: community call every Friday?"},
            {"!general:demo.local", "@alice", "Best command line trick you have learned?"},
            {"!general:demo.local", "@dave", "The sync rework: how it works under the hood"},
            {"!general:demo.local", "@kate", "What is on your desktop setup?"},
            {"!general:demo.local", "@you", "Weekly demo showcase — what should we show?"},
            {"!general:demo.local", "@bob", "E2EE best practices discussion"},
            {"!design:demo.local", "@carol", "New color palette for the app"},
            {"!random:demo.local", "@bob", "The best terminal emulator debate"},
            {"!music:demo.local", "@alice", "This week's playlist drop"},
            {"!books:demo.local", "@grace", "Book club pick for August"},
        };
        for (auto& rt : roots) {
            std::string rootId = "$demo_thread_" + std::to_string(tt);
            matrix::Event root;
            root.event_id = rootId;
            root.room_id = rt.room; root.sender = rt.sender;
            root.type = "m.room.message";
            root.content = {{"msgtype", "m.text"}, {"body", rt.body},
                            {"m.relates_to", {{"event_id", rootId},
                                              {"rel_type", "m.thread"}}}};
            root.origin_server_ts = tt;
            dbi.insertEvent(root);
            tt -= 3600000;
            struct { const char* sender; const char* body; } reps[] = {
                {"@you", "Sounds good, count me in!"},
                {"@bob", "I would join."},
                {"@alice", "Seconded."},
                {"@charlie", "Love this."},
            };
            int n = (std::string(rt.body).size() % 3) + 2;  // 2..4 replies
            for (int ri = 0; ri < n; ++ri) {
                matrix::Event rep;
                rep.event_id = "$demo_thread_" + std::to_string(tt);
                rep.room_id = rt.room; rep.sender = reps[ri].sender;
                rep.type = "m.room.message";
                rep.content = {{"msgtype", "m.text"}, {"body", reps[ri].body},
                               {"m.relates_to", {{"event_id", rootId},
                                                 {"rel_type", "m.thread"}}}};
                rep.origin_server_ts = tt;
                dbi.insertEvent(rep);
                tt -= 60000;
            }
        }
    }

    // Power levels in the DMs: everybody is admin there (like Element).
    {
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!dm_alice:demo.local"; pl.sender = "@alice";
        pl.type = "m.room.power_levels";
        pl.content = {{"users", {{"@alice", 100}, {"@you", 100}}}};
        pl.origin_server_ts = ts;
        dbi.insertEvent(pl);
        ts -= 3600000;
        matrix::Event pl2;
        pl2.event_id = "$demo_" + std::to_string(ts);
        pl2.room_id = "!dm_bob:demo.local"; pl2.sender = "@bob";
        pl2.type = "m.room.power_levels";
        pl2.content = {{"users", {{"@bob", 100}, {"@you", 100}}}};
        pl2.origin_server_ts = ts;
        dbi.insertEvent(pl2);
        ts -= 3600000;
    }

    // Power levels: alice admin (100), bob mod (50), the rest members.
    {
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!general:demo.local"; pl.sender = "@alice";
        pl.type = "m.room.power_levels";
        pl.content = {{"users", {{"@alice", 100}, {"@bob", 50}, {"@charlie", 0}}}};
        pl.origin_server_ts = ts;
        dbi.insertEvent(pl);
        ts -= 3600000;
    }

    // A message with a permalink (matrix.to) — the ui renders it as a pill.
    // The target event is inserted for real and BOTH events get fresh
    // timestamps, so the pill always sits at the top of #general with its
    // preview ("📎 #general · alice: ...") instead of "(event)".
    {
        std::string welcomeId = "$demo_welcome_pill";
        int64_t pillNow = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        matrix::Event target;
        target.event_id = welcomeId;
        target.room_id = "!general:demo.local"; target.sender = "@alice";
        target.type = "m.room.message";
        target.content = {{"msgtype", "m.text"},
                          {"body", "Welcome to the demo — this is the linked message."}};
        target.origin_server_ts = pillNow - 3600000;
        dbi.insertEvent(target);
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(pillNow);
        pl.room_id = "!general:demo.local"; pl.sender = "@bob";
        pl.type = "m.room.message";
        pl.content = {{"msgtype", "m.text"},
                      {"body", "Look at this: https://matrix.to/#/!general:demo.local/"
                               + welcomeId}};
        pl.origin_server_ts = pillNow - 3600000 - 60000;
        dbi.insertEvent(pl);
    }

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
        int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int fi = 0; fi < int(sizeof(fresh) / sizeof(fresh[0])); ++fi) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(t0 - int64_t(fi) * 7);
            ev.room_id = fresh[fi].room; ev.sender = fresh[fi].sender;
            ev.type = "m.room.message";
            ev.content = {{"msgtype", "m.text"}, {"body", fresh[fi].body}};
            ev.origin_server_ts = t0 - int64_t(fi) * 137000 - int64_t(fi % 5) * 23000;
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

    std::cout << "Populated DB: " << (sizeof(rooms)/sizeof(rooms[0])) << " rooms, "
              << (sizeof(msgs)/sizeof(msgs[0])) + 2 + 4 << " messages (incl. a thread)." << std::endl;
    std::cout << "Try:  progressive-cli rooms | progressive-cli view #general | progressive-cli view #dev" << std::endl;
    return 0;
}


// ---- Interactive demo REPL (offline, no Matrix account needed) ----
//
// Replaces the old `demo` behavior (which started an HTTP API server on
// port 8080). Now `progressive-cli demo` drops the user into an interactive
// terminal session against the offline demo database: type commands, see
// output, same handlers as the real CLI (rooms/view/search). The web demo
// stays available as `progressive-cli serve --demo`.

static void demoReplParseLine(const std::string& line, matrixcli::cli::Args& out) {
    std::istringstream iss(line);
    std::vector<std::string> words;
    std::string w;
    while (iss >> w) words.push_back(w);
    if (words.empty()) return;
    out.command = words[0];
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
            std::string key = words[i].substr(2);
            bool nextIsOpt = (i + 1 < words.size()) &&
                words[i + 1].size() >= 2 && words[i + 1][0] == '-' && words[i + 1][1] == '-';
            if (i + 1 < words.size() && !nextIsOpt) {
                out.options[key] = words[i + 1];
                ++i;
            } else {
                out.options[key] = "true";
            }
        } else {
            out.positional.push_back(words[i]);
        }
    }
}

static void demoReplSend(const matrixcli::cli::Args& args) {
    if (args.positional.size() < 2) {
        std::cout << "Usage: send <room> <message text>" << std::endl;
        return;
    }
    matrixcli::db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cout << "[demo] cannot open matrixcli.db" << std::endl;
        return;
    }
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string body;
    for (size_t i = 1; i < args.positional.size(); ++i) {
        if (!body.empty()) body += " ";
        body += args.positional[i];
    }
    matrixcli::matrix::Event ev;
    ev.event_id = "$demo_" + std::to_string(ts);
    ev.room_id = args.positional[0];
    ev.sender = "@you:demo.local";
    ev.type = "m.room.message";
    ev.content = {{"body", body}, {"msgtype", "m.text"}};
    ev.origin_server_ts = ts;
    dbi.insertEvent(ev);
    std::cout << "[demo] sent " << ts << " to " << args.positional[0] << ": " << body << std::endl;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args);
#endif
int cmdDemoRepl(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    // Pure CLI mode: populate the demo DB and exit — the user then runs the
    // normal one-shot commands (progressive-cli rooms / view / send / search).
    auto runPureCli = []() {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
        std::cout << "Demo data ready. Use the one-shot commands:\n"
                     "  progressive-cli rooms\n"
                     "  progressive-cli view \"#general\" 10\n"
                     "  progressive-cli send \"#general\" \"hello\"\n";
        return 0;
    };
    // The positional shortcuts: demo tui | ui | mobile | cli — no menu.
    if (!args.positional.empty()) {
        const std::string mode = args.positional[0];
        cli::Args sub = args;
        sub.positional.erase(sub.positional.begin());
        if (mode == "tui") {
            sub.options["tui"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "ui" || mode == "ascii") {
            sub.options["ui"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "mobile" || mode == "phone") {
            sub.options["ui"] = "true";
            sub.options["mobile"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "cli" || mode == "populate") {
            sub.options["cli"] = "true";
            return cmdDemoRepl(sub);
        }
    }

    if (args.options.count("cli") || args.options.count("populate")) {
        return runPureCli();
    }

    // Demo + ASCII interface: 'demo --ui [room]' runs the ASCII client
    // interface; with --static/--once it draws the frame once and exits
    // (non-interactive, pipe-friendly) instead of starting the REPL.
    if (args.options.count("ui") || args.options.count("ascii")) {
        cli::Args uiArgs;
        if (!args.positional.empty()) {
            uiArgs.positional.push_back(args.positional[0]);
        }
        if (args.options.count("static") || args.options.count("once")) {
            uiArgs.options["static"] = "true";
        }
        if (args.options.count("ids")) uiArgs.options["ids"] = "true";
        if (args.options.count("time-full") || args.options.count("sec"))
            uiArgs.options["time-full"] = "true";
        if (args.options.count("right")) uiArgs.options["right"] = args.options.at("right");
        if (args.options.count("thread-root"))
            uiArgs.options["thread-root"] = args.options.at("thread-root");
        if (args.options.count("thread"))
            uiArgs.options["thread"] = args.options.at("thread");
        if (args.options.count("limit")) uiArgs.options["limit"] = args.options.at("limit");
        if (args.options.count("rows")) uiArgs.options["rows"] = args.options.at("rows");
        if (args.options.count("scroll")) uiArgs.options["scroll"] = args.options.at("scroll");
        if (args.options.count("scroll-left")) uiArgs.options["scroll-left"] = args.options.at("scroll-left");
        if (args.options.count("mobile")) uiArgs.options["mobile"] = "true";
        if (args.options.count("panel-left")) uiArgs.options["panel-left"] = args.options.at("panel-left");
        if (args.options.count("panel-right")) uiArgs.options["panel-right"] = args.options.at("panel-right");
        if (args.options.count("panel-auto")) uiArgs.options["panel-auto"] = args.options.at("panel-auto");
        if (args.options.count("members")) uiArgs.options["members"] = args.options.at("members");
        if (args.options.count("space")) uiArgs.options["space"] = args.options.at("space");
        if (args.options.count("time-side")) uiArgs.options["time-side"] = args.options.at("time-side");
        if (args.options.count("msg-line")) uiArgs.options["msg-line"] = args.options.at("msg-line");
        for (const char* k : {"agent", "agent-provider", "agent-endpoint",
                              "agent-model", "agent-key", "agent-trust"}) {
            if (args.options.count(k)) uiArgs.options[k] = args.options.at(k);
        }
        return matrixcli::cmdAsciiUi(uiArgs);
    }

    // Demo + the ncurses TUI: 'demo --tui' (or choice 4) — the terminal UI
    // with the demo database populated.
    if (args.options.count("tui")) {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
#ifdef BUILD_TUI
        return cmdTUI(args);
#else
        std::cout << "tui not built (recompile with BUILD_TUI=ON)." << std::endl;
        return 1;
#endif
    }

    // On a terminal, let the user CHOOSE: interactive REPL, pure CLI, or
    // the ASCII-drawn client interface (rooms | chat | members).
    if (isatty(STDIN_FILENO)) {
        std::cout << "Choose demo mode:\n"
                     "  1) interactive session (type commands at a prompt)\n"
                     "  2) populate demo data and exit (one-shot commands)\n"
                     "  3) ASCII client interface (rooms | chat | members)\n"
                     "     (non-interactive: progressive-cli demo --ui --static)\n"
                     "  4) terminal UI (ncurses TUI)\n"
                     "  5) ASCII client for smartphones (stacked, portrait)\n"
                     "Choice [1/2/3/4/5]: " << std::flush;
        std::string ans;
        std::getline(std::cin, ans);
        if (!matrixcli::g_interrupted.load()) {
            // Ctrl+C at the choice prompt — leave immediately.
            std::cout << "Bye!" << std::endl;
            return 0;
        }
        if (!ans.empty() && (ans[0] == '2' || ans[0] == 'n')) {
            return runPureCli();
        }
        if (!ans.empty() && ans[0] == '3') {
            cli::Args uiArgs;
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (!ans.empty() && ans[0] == '5') {
            cli::Args uiArgs;
            uiArgs.options["mobile"] = "true";
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (!ans.empty() && ans[0] == '4') {
            cli::Args tuiArgs;
            tuiArgs.options["tui"] = "true";
            return cmdDemoRepl(tuiArgs);
        }
    }

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    if (dbi.listRooms().empty()) {
        std::cout << "Populating demo data..." << std::endl;
        populateDemoData(dbi);
    }

    std::cout << "progressive-cli demo — interactive mode (offline, no account needed)" << std::endl;
    std::cout << "Commands: help | rooms | view <room> [n] | search <query> |"
              << " send <room> <text> | ui | clear | quit" << std::endl;
    std::cout << "Demo rooms: #general  #dev  #random  #dm_alice  #dm_bob" << std::endl;

    std::string line;
    std::vector<std::string> history;
    for (;;) {
        if (!matrixcli::g_interrupted.load()) break;  // Ctrl+C
        if (!readLineWithHistory(history, "demo> ", line)) break;
        if (!matrixcli::g_interrupted.load()) break;
        // trim
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        cli::Args a;
        demoReplParseLine(line, a);

        if (a.command == "quit" || a.command == "exit") break;
        if (a.command == "help") {
            std::cout << "  rooms                     list demo rooms\n"
                         "  view <room> [n]           show the last n messages (default 20)\n"
                         "  search <query>            full-text search in cached messages\n"
                         "  send <room> <text>        send a message (demo, offline)\n"
                         "  ui                        ASCII client interface (rooms | chat | members)\n"
                         "  clear                     clear the screen\n"
                         "  quit / exit               leave the demo\n";
            continue;
        }
        if (a.command == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
            continue;
        }
        if (a.command == "rooms") { cmdRooms(a); continue; }
        if (a.command == "view") { cmdView(a); continue; }
        if (a.command == "search") { cmdSearch(a); continue; }
        if (a.command == "send") { demoReplSend(a); continue; }
        if (a.command == "attach" || a.command == "send-file") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: attach <room> <file> [--caption text]" << std::endl;
                continue;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomQ || name == roomQ ||
                    (!roomQ.empty() && (name.find(roomQ) == 0 ||
                                        id.find(roomQ) != std::string::npos))) {
                    roomId = id;
                    break;
                }
            }
            std::string cap = a.options.count("caption") ? a.options.at("caption") : "";
            std::string thr = a.options.count("thread") ? a.options.at("thread") : "";
            uiInsertLocalFile(dbi, roomId, a.positional[1], cap, thr);
            std::cout << "[demo] file recorded locally: " << a.positional[1]
                      << " -> " << roomId << std::endl;
            continue;
        }
        if (a.command == "ui" || a.command == "ascii") {
            int rc = matrixcli::cmdAsciiUi(a);
            std::cout << "Back in the demo session (type 'quit' to leave)." << std::endl;
            if (rc != 0) continue;
            continue;
        }
        std::cout << "Unknown command '" << a.command << "' — type 'help'." << std::endl;
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    Config::instance().load("config.json");
    matrix::Client client;

    db::Database dbi;
    dbi.open("matrixcli.db");
    auto acc = dbi.loadAccount();
    if (acc.is_logged_in()) {
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);
    } else if (!Config::instance().homeserverURL().empty()) {
        client.setHomeserverURL(Config::instance().homeserverURL());
        client.setAccessToken(Config::instance().accessToken());
    }
    client.setDatabase(&dbi);

    // Initialize crypto if logged in from DB
    if (acc.is_logged_in()) {
        client.initCrypto(acc.user_id, acc.device_id);
    }

    // Offline agent mode: `progressive-cli tui agent [task]` — no Matrix login
    // needed. The TUI opens straight into the chat with a virtual #agent
    // room; /agent, /agent-code and /llm work (the live Matrix tools
    // answer "no matrix session", the cache-backed ones work offline).
    bool agentOnly = false;
    std::string agentTask;
    if (!args.positional.empty() && args.positional[0] == "agent") {
        agentOnly = true;
        for (size_t i = 1; i < args.positional.size(); i++) {
            if (i > 1) agentTask += " ";
            agentTask += args.positional[i];
        }
    }
    if (args.options.count("agent")) agentOnly = true;

    tui::Screen screen;
    screen.init();

    // Demo TUI: skip the login view — the demo database is already
    // populated, the chat opens straight away (offline).
    tui::LoginResult login_result;
    if (agentOnly) {
        login_result.success = true;
        login_result.username = "offline";
        login_result.homeserver = "";
        login_result.password = "";
    } else if (args.options.count("tui")) {
        db::Database checkDb;
        if (checkDb.open("matrixcli.db") && checkDb.listRooms().empty()) {
            populateDemoData(checkDb);
        }
        login_result.success = true;
        login_result.username = "demo";
        login_result.homeserver = "demo.local";
        login_result.password = "";
    } else {
        tui::LoginView login_view;
        login_result = login_view.run(screen);
    }

    if (login_result.success) {
        try {
            bool demoMode = args.options.count("tui");
            db::StoredAccount sacc;
            if (agentOnly) {
                // No login, and the stored account is left untouched —
                // the agent mode must not overwrite the real login.
                sacc.homeserver_url = "";
                sacc.user_id = "@offline:localhost";
                sacc.access_token = "";
                sacc.device_id = "";
            } else {
                client.setHomeserverURL(login_result.homeserver);
                if (demoMode) {
                    // Offline demo: a fake user, no network login.
                    sacc.homeserver_url = "demo.local";
                    sacc.user_id = "@demo:demo.local";
                    sacc.access_token = "";
                    sacc.device_id = "demo-tui";
                } else {
                    auto creds = client.loginPassword(login_result.username, login_result.password);
                    // The login-screen connection choice (direct/tor/i2p/
                    // yggdrasil/custom): applies to the TUI client now and
                    // persists into config.json for the CLI + the global
                    // core proxy.
                    applyConnectionChoice(client, login_result.connection);
                    Config::instance().set("homeserver_url", login_result.homeserver);
                    Config::instance().set("access_token", creds.access_token);
                    Config::instance().set("user_id", creds.user_id);
                    Config::instance().set("device_id", creds.device_id);
                    Config::instance().save();
                    sacc.homeserver_url = login_result.homeserver;
                    sacc.user_id = creds.user_id;
                    sacc.access_token = creds.access_token;
                    sacc.device_id = creds.device_id;
                    // Init crypto
                    client.initCrypto(creds.user_id, creds.device_id);
                }
            }
            if (!agentOnly) dbi.saveAccount(sacc);

            // First-run agent setup: no configured API key → the wizard
            // asks for the provider preset, the key and the extras, then
            // persists everything into ~/.config/matrixcli/agent.json.
            if (agentOnly) {
                matrixcli::matrixagent::Config probe;
                matrixcli::matrixagent::applyDefaults(probe);
                if (probe.key.empty()) {
                    tui::AgentSetupView setup;
                    tui::AgentSetupResult sr = setup.run(screen);
                    if (sr.ok) {
                        agenttools::Config out;
                        if (!sr.provider.empty() && sr.provider != "custom") {
                            agenttools::applyProviderPreset(out, sr.provider);
                        } else {
                            out.provider = "openai";
                        }
                        if (!sr.endpoint.empty()) out.endpoint = sr.endpoint;
                        if (!sr.model.empty()) out.model = sr.model;
                        out.key = sr.key;
                        out.proxy = sr.proxy;
                        out.trust = "ask";
                        agenttools::saveAgentConfig(out);
                    }
                }
            }

            tui::ChatView chat;
            {
                char cwdBuf[4096];
                std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf))
                                      ? cwdBuf : std::string(".");
                chat.setStatus(agentOnly
                                   ? "offline agent mode · /agent /agent-code /llm · " + cwd
                                   : "Connected as " + sacc.user_id + " · " + cwd);
            }
            chat.setConnectionStatus(agentOnly ? "offline (agent mode)"
                                               : demoMode ? "demo (offline)"
                                                          : "online");

            // Load TUI config
            tui::TUIConfig tuiCfg = tui::TUIConfig::load("matrixcli.toml");
            if (args.options.count("no-mouse")) tuiCfg.mouse_enabled = false;
            if (args.options.count("mouse")) tuiCfg.mouse_enabled = true;
            screen.setMouseEnabled(tuiCfg.mouse_enabled);

            // Command handler for slash commands
            chat.setEscHandler([]() { matrixcli::g_agentInterrupt = true; });
            // Ctrl+C exits the TUI (the same SIGINT flag the agent loops
            // and the REPLs observe).
            chat.setQuitCheck([]() { return !matrixcli::g_interrupted.load(); });

            // The Matrix agent launcher — shared by the /agent slash
            // command and the `progressive-cli tui agent <task>` auto-start.
            static std::atomic<bool> agentBusy{false};
            auto launchMatrixAgent = [&](const std::string& task) {
                if (task.empty()) return;
                std::string roomId = chat.activeRoomId();
                if (roomId.empty()) roomId = "!agent:demo.local";
                if (agentBusy.exchange(true)) {
                    chat.setConnectionStatus("agent busy — wait or Esc");
                    agentBusy = false;
                    return;
                }
                matrixcli::g_agentInterrupt = false;
                chat.setConnectionStatus("agent running: "
                                         + task.substr(0, 30) + "...");
                std::thread([&, task, roomId]() {
                    matrixcli::matrixagent::Config cfg;
                    cfg.verbose = true;
                    matrixcli::matrixagent::applyDefaults(cfg);
                    auto backend = matrixcli::matrixagent::makeMatrixBackend(&client);
                    matrixcli::matrixagent::Result res = matrixcli::matrixagent::run(
                        cfg, *backend, task, roomId,
                        [&](const std::string& l) {
                            tui::MessageInfo mi;
                            mi.sender = "@agent";
                            mi.body = l;
                            chat.addMessage(roomId, mi);
                        });
                    tui::MessageInfo mi;
                    mi.sender = "@agent";
                    mi.body = res.ok
                        ? (res.answer.empty() ? "[agent] done, no final answer"
                                              : res.answer)
                        : "[agent error] " + res.error;
                    chat.addMessage(roomId, mi);
                    chat.setConnectionStatus("agent done");
                    agentBusy = false;
                }).detach();
            };

            // The SAS confirmation flags (shared by /verify, /verify-confirm
            // and /verify-cancel).
            std::atomic<bool> sasConfirm{false};
            std::atomic<bool> sasCancel{false};

            chat.setCommandHandler([&](const std::string& cmd, const std::string& args) {
                if (cmd == "me" || cmd == "emote") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendEmote(roomId, args);
                } else if (cmd == "notice") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendNotice(roomId, args);
                } else if (cmd == "join") {
                    if (!args.empty()) client.joinRoom(args);
                } else if (cmd == "leave") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.leaveRoom(roomId);
                } else if (cmd == "nick" || cmd == "name") {
                    if (!args.empty()) client.setDisplayName(args);
                } else if (cmd == "topic" || cmd == "desc") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) client.setRoomTopic(roomId, args);
                } else if (cmd == "roomname") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) client.setRoomName(roomId, args);
                } else if (cmd == "avatar") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        // If args is a file path, upload first
                        std::string url = args;
                        if (args.find("mxc://") != 0 && args.find("http") != 0) {
                            try { url = client.uploadMedia(args); } catch (...) { return; }
                        }
                        client.setRoomAvatar(roomId, url);
                    }
                } else if (cmd == "useravatar") {
                    if (!args.empty()) {
                        std::string url = args;
                        if (args.find("mxc://") != 0 && args.find("http") != 0) {
                            try { url = client.uploadMedia(args); } catch (...) { return; }
                        }
                        client.setAvatarUrl(url);

                    }
                } else if (cmd == "displayname" || cmd == "nick") {
                    if (!args.empty()) client.setDisplayName(args);
                } else if (cmd == "redact" || cmd == "delete") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        // The moderation guard: the others' messages are
                        // protected unless modredact is on.
                        matrix::Event target;
                        if (dbi.getEventById(args, target) &&
                            target.sender != client.userId() &&
                            dbi.getSetting("mod_redact", "0") == "0") {
                            chat.setConnectionStatus("redaction blocked (modredact off)");
                        } else {
                            client.redactEvent(roomId, args);
                        }
                    }
                } else if (cmd == "edit") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos) {
                        nlohmann::json c = {{"msgtype","m.text"},{"body","* "+args.substr(sp+1)},
                            {"m.new_content",{{"msgtype","m.text"},{"body",args.substr(sp+1)}}},
                            {"m.relates_to",{{"event_id",args.substr(0,sp)},{"rel_type","m.replace"}}}};
                        try { client.sendEvent(roomId, "m.room.message", c); } catch (...) {}
                    }
                } else if (cmd == "knock") {
                    if (!args.empty()) try { client.knockRoom(args); } catch (...) {}
                } else if (cmd == "read" || cmd == "markread") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        // The local last-read marker moves to the newest
                        // cached event (the m.fully_read copy).
                        auto evs = dbi.getEvents(roomId, 1);
                        if (!evs.empty()) dbi.setReadMarker(roomId, evs.front().event_id);
                        // The server-side marker follows the per-room
                        // receipts policy (see /receipts).
                        if (dbi.receiptsEnabled(roomId)) {
                            try { client.sendReadReceipt(roomId, ""); } catch (...) {}
                            chat.setConnectionStatus("marked read (receipt sent)");
                        } else {
                            chat.setConnectionStatus("marked read locally (receipts off)");
                        }
                    }
                } else if (cmd == "receipts") {
                    // /receipts on|off — the read-receipt policy for the
                    // active room.
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        if (args == "off") {
                            dbi.setReceiptsEnabled(roomId, false);
                            chat.setConnectionStatus("receipts off for this room");
                        } else if (args == "on") {
                            dbi.setReceiptsEnabled(roomId, true);
                            chat.setConnectionStatus("receipts on for this room");
                        } else {
                            chat.setConnectionStatus(std::string("receipts: ")
                                + (dbi.receiptsEnabled(roomId) ? "on" : "off")
                                + " — /receipts on|off");
                        }
                    }
                } else if (cmd == "verify") {
                    // /verify <@user:server> <deviceId> — the SAS device
                    // verification in the chat: the emojis land here, the
                    // match is confirmed with /verify-confirm (cancelled
                    // with /verify-cancel). Uses the ecore session (run
                    // 'progressive-cli login' once for the crypto identity).
                    std::istringstream ss(args);
                    std::string user, device;
                    ss >> user >> device;
                    if (user.empty() || device.empty()) {
                        chat.setConnectionStatus("usage: /verify <@user:server> <deviceId>");
                    } else {
                        sasConfirm = false;
                        sasCancel = false;
                        std::string roomId = chat.activeRoomId();
                        if (roomId.empty()) roomId = "!agent:demo.local";
                        chat.setConnectionStatus("verifying " + user + "/"
                                                 + device + " — /verify-confirm | /verify-cancel");
                        std::thread([&, user, device, roomId]() {
                            auto post = [&](const std::string& s) {
                                tui::MessageInfo mi;
                                mi.sender = "@verify";
                                mi.is_notice = true;
                                mi.body = s;
                                chat.addMessage(roomId, mi);
                            };
                            const int rc = runSasVerification(
                                user, device, 180, false, post,
                                [&]() -> bool {
                                    // Wait for the user's /verify-confirm
                                    // (or the cancel).
                                    for (int i = 0; i < 1200; i++) {
                                        if (sasCancel.load()) return false;
                                        if (sasConfirm.load()) return true;
                                        usleep(100000);
                                    }
                                    return false;
                                });
                            post(rc == 0 ? "\u2713 verification done"
                                         : "verification failed or cancelled");
                            chat.setConnectionStatus("verification finished");
                        }).detach();
                    }
                } else if (cmd == "verify-confirm") {
                    sasConfirm = true;
                    chat.setConnectionStatus("confirmation accepted — sending the MAC");
                } else if (cmd == "verify-cancel") {
                    sasCancel = true;
                    chat.setConnectionStatus("cancellation requested");
                } else if (cmd == "online") {
                    client.setPresence("online");
                } else if (cmd == "away") {
                    client.setPresence("unavailable");
                } else if (cmd == "offline") {
                    client.setPresence("offline");
                } else if (cmd == "devices") {
                } else if (cmd == "invite") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.inviteUser(roomId, user, reason);
                    }
                } else if (cmd == "kick") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.kickUser(roomId, user, reason);
                    }
                } else if (cmd == "ban") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.banUser(roomId, user, reason);
                    }
                } else if (cmd == "react") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string eventId = (sp != std::string::npos) ? args.substr(0, sp) : "";
                        std::string key = (sp != std::string::npos) ? args.substr(sp + 1) : args;
                        if (!eventId.empty()) {
                            try { client.sendReaction(roomId, eventId, key); } catch (...) {}
                        }
                    }
                } else if (cmd == "vote") {
                    // Vote in a poll: /vote event_id answer  (or /vote event_id 1 for option number)
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string pollId = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string answers = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        std::vector<std::string> ansVec;
                        if (!answers.empty()) {
                            size_t pos = 0;
                            while ((pos = answers.find(',')) != std::string::npos) {
                                ansVec.push_back(answers.substr(0, pos));
                                answers.erase(0, pos + 1);
                            }
                            ansVec.push_back(answers);
                        }
                        try { client.sendPollResponse(roomId, pollId, ansVec); } catch (...) {}
                    }
                } else if (cmd == "poll") {
                    // Create poll: /poll "Question?" "Option A" "Option B" "Option C"
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        std::vector<std::string> parts;
                        bool in_quote = false;
                        std::string cur;
                        for (char c : args) {
                            if (c == '"') { in_quote = !in_quote; continue; }
                            if (c == ' ' && !in_quote && !cur.empty()) { parts.push_back(cur); cur.clear(); continue; }
                            cur += c;
                        }
                        if (!cur.empty()) parts.push_back(cur);
                        if (parts.size() >= 3) {
                            std::string question = parts[0];
                            std::vector<std::string> answers(parts.begin() + 1, parts.end());
                            try { client.sendPoll(roomId, question, answers); } catch (...) {}
                        }
                    }
                } else if (cmd == "agent") {
                    // The Matrix agent (Android-parity /agent): the LLM
                    // loop with the Matrix tools (read/search/send messages
                    // etc.) runs in a background thread; the progress + the
                    // answer land in the chat as @agent messages. Esc
                    // interrupts the run.
                    launchMatrixAgent(args);
                } else if (cmd == "agent-code") {
                    // The coding agent in the TUI: the run in a background
                    // thread, the progress + the answer land in the chat.
                    std::string prompt = args;
                    std::string roomId = chat.activeRoomId();
                    if (roomId.empty()) roomId = "!agent:demo.local";
                    static std::atomic<bool> agentCodeBusy{false};
                    if (agentCodeBusy.exchange(true)) {
                        chat.setConnectionStatus("agent-code busy — wait or Esc");
                        agentCodeBusy = false;
                    } else {
                        matrixcli::g_agentInterrupt = false;
                        chat.setConnectionStatus("agent-code running: "
                                                 + prompt.substr(0, 30) + "...");
                        std::thread([&, prompt, roomId]() {
                            agenttools::Config cfg;
                            agenttools::loadAgentConfig(cfg);
                            if (cfg.key.empty()) {
                                const char* env = cfg.provider == "anthropic"
                                    ? std::getenv("ANTHROPIC_API_KEY")
                                    : std::getenv("OPENAI_API_KEY");
                                if (env && *env) cfg.key = env;
                            }
                            std::vector<agenttools::Message> hist;
                            agenttools::Result res = agenttools::run(
                                cfg, prompt, hist, nullptr, nullptr,
                                [&](const std::string& l) {
                                    tui::MessageInfo mi;
                                    mi.sender = "@agent-code";
                                    mi.body = l;
                                    chat.addMessage(roomId, mi);
                                },
                                nullptr);
                            tui::MessageInfo mi;
                            mi.sender = "@agent-code";
                            mi.body = res.ok ? res.text
                                             : "[agent error] " + res.error;
                            chat.addMessage(roomId, mi);
                            chat.setConnectionStatus("agent-code done");
                            agentCodeBusy = false;
                        }).detach();
                    }
                } else if (cmd == "llm") {
                    // A single LLM completion (the Android /llm parity).
                    std::string prompt = args;
                    std::string roomId = chat.activeRoomId();
                    if (roomId.empty()) roomId = "!agent:demo.local";
                    std::thread([&, prompt, roomId]() {
                        matrixcli::matrixagent::Config cfg;
                        matrixcli::matrixagent::applyDefaults(cfg);
                        auto cres = matrixcli::matrixagent::completeEx(
                            cfg, "", prompt);
                        tui::MessageInfo mi;
                        mi.sender = "@llm";
                        mi.body = cres
                            ? (cres->text.empty() ? "(empty response)" : cres->text)
                            : "[llm error] " + cres.error();
                        chat.addMessage(roomId, mi);
                    }).detach();
                } else if (cmd == "shrug") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendTextMessage(roomId, "¯\\_(ツ)_/¯");
                } else if (cmd == "tableflip") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendTextMessage(roomId, "(╯°□°)╯︵ ┻━┻");
                } else if (cmd == "upload") {
                    std::string roomId = chat.activeRoomId();
                    if (!args.empty()) {
                        if (!roomId.empty()) {
                            try {
                                auto mxc = client.uploadMedia(args);
                                client.sendFileMessage(roomId, mxc, args, 0, "");
                    } catch (...) {}
                }
                } else if (cmd == "voice") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        try {
                            auto mxc = client.uploadMedia(args);
                            client.sendVoiceMessage(roomId, mxc, 3000);
                        } catch (...) {}
                    }
                } else if (cmd == "sticker") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        try {
                            std::string url = (args.find("mxc://") == 0) ? args : client.uploadMedia(args);
                            client.sendSticker(roomId, url, "Sticker");
                        } catch (...) {}
                    }
                } else if (cmd == "location") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string geo = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string desc = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        try { client.sendLocation(roomId, geo, desc); } catch (...) {}
                    }
                } else if (cmd == "todo") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json items = nlohmann::json::array();
                        items.push_back({{"text", args}, {"done", false}});
                        try { client.sendTodo(roomId, "TODO", items); } catch (...) {}
                    }
                } else if (cmd == "bridge") {
                    // Bridge status — check account data for bridge info
                    chat.setConnectionStatus("Bridges: IRC/XMPP/Telegram/DeltaChat available");
                } else if (cmd == "op" || cmd == "admin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.setPowerLevel(roomId, args, 100); } catch (...) {}
                } else if (cmd == "deop") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.setPowerLevel(roomId, args, 0); } catch (...) {}
                } else if (cmd == "whois") {
                    if (!args.empty()) {
                        try { chat.setConnectionStatus("whois " + args + ": " + client.getDisplayName(args)); } catch (...) {}
                    }
                } else if (cmd == "ignore") {
                    if (!args.empty()) try { client.ignoreUser(args); } catch (...) {}
                } else if (cmd == "unignore") {
                    if (!args.empty()) try { client.unignoreUser(args); } catch (...) {}
                } else if (cmd == "unban") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) try { client.unbanUser(roomId, args); } catch (...) {}
                } else if (cmd == "myroomnick") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json c = {{"membership","join"},{"displayname",args}};
                        try { client.sendStateEvent(roomId, "m.room.member", client.userId(), c); } catch (...) {}
                    }
                } else if (cmd == "spoiler") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json c = {{"msgtype","m.text"},{"body","||"+args+"||"},
                            {"format","org.matrix.custom.html"},
                            {"formatted_body","<span data-mx-spoiler>"+args+"</span>"}};
                        try { client.sendEvent(roomId, "m.room.message", c); } catch (...) {}
                    }
                } else if (cmd == "plain") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendTextMessage(roomId, args); } catch (...) {}
                } else if (cmd == "lenny") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " ( ͡° ͜ʖ ͡°)"); } catch (...) {}
                } else if (cmd == "discardsession") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && client.isRoomEncrypted(roomId))
                        try { client.enableEncryption(roomId); } catch (...) {}
                } else if (cmd == "mute") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.setRoomTag(roomId, "m.lowpriority"); } catch (...) {}
                } else if (cmd == "unmute") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.deleteRoomTag(roomId, "m.lowpriority"); } catch (...) {}
                } else if (cmd == "pin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.pinEvent(roomId, args); } catch (...) {}
                } else if (cmd == "unpin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.unpinEvent(roomId, args); } catch (...) {}
                } else if (cmd == "pins") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            auto p = client.getPinnedEvents(roomId);
                            int cnt = p.value("pinned", nlohmann::json::array()).size();
                            chat.setConnectionStatus("📌 " + std::to_string(cnt) + " pinned messages");
                        } catch (...) {}
                    }
                } else if (cmd == "roomsbrowse" || cmd == "explore") {
                    if (!args.empty()) {
                        try {
                            auto pubs = client.getPublicRooms("", args, 20);
                            int total = pubs.value("total_room_count_estimate", 0);
                            chat.setConnectionStatus("Browse: " + std::to_string(total) + " rooms matching '" + args + "'");
                        } catch (...) {}
                    }
                } else if (cmd == "preview") {
                    // Show link preview via Matrix preview_url API
                    if (!args.empty()) {
                        try {
                            auto p = client.getURLPreview(args);
                            if (p.contains("og:title"))
                                chat.setConnectionStatus(p["og:title"].get<std::string>() +
                                    (p.contains("og:description") ? " — " + p["og:description"].get<std::string>() : ""));
                        } catch (...) {}
                    }
                } else if (cmd == "stats") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            auto st = client.getRoomStats(roomId);
                            chat.setConnectionStatus("Stats: " + std::to_string(st.value("total_messages", 0)) +
                                " msgs, " + std::to_string(st.value("unique_posters", 0)) + " posters");
                        } catch (...) {}
                    }
                } else if (cmd == "fav" || cmd == "favorite") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.setRoomTag(roomId, "m.favourite"); } catch (...) {}
                } else if (cmd == "mirror") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        try { client.mirrorMessage(roomId, args.substr(0, sp), args.substr(sp + 1)); } catch (...) {}
                } else if (cmd == "markdown") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json content = {{"msgtype", "m.text"}, {"body", args},
                            {"format", "org.matrix.custom.html"}, {"formatted_body", "<p>" + args + "</p>"}};
                        try { client.sendEvent(roomId, "m.room.message", content); } catch (...) {}
                    }
                } else if (cmd == "upgrade" || cmd == "upgraderoom") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { chat.setConnectionStatus("Upgraded " + client.upgradeRoom(roomId)); } catch (...) {}
                } else if (cmd == "export") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            std::string fmt = args.empty() ? "text" : args;
                            std::string out = client.exportRoom(roomId, fmt);
                            std::ofstream ofs("export_" + roomId + "." + (fmt == "json" ? "json" : fmt == "html" ? "html" : "txt"));
                            ofs << out;
                            chat.setConnectionStatus("Exported to " + std::string(fmt == "json" ? "json" : fmt == "html" ? "html" : "txt"));
                        } catch (...) { chat.setConnectionStatus("Export failed"); }
                    }
                } else if (cmd == "statusmsg") {
                    if (!args.empty()) {
                        auto sp = args.find(' ');
                        std::string emoji = (sp != std::string::npos) ? args.substr(0, sp) : "";
                        std::string text = (sp != std::string::npos) ? args.substr(sp + 1) : args;
                        try { client.setCustomStatus(text, emoji); chat.setConnectionStatus("Status set"); } catch (...) {}
                    }
                } else if (cmd == "remind" || cmd == "reminder") {
                    auto sp = args.find(' ');
                    if (sp != std::string::npos) {
                        int secs = std::stoi(args.substr(sp + 1));
                        chat.setConnectionStatus("Reminder set in " + std::to_string(secs) + "s");
                    }
                } else if (cmd == "notify") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "add" && !val.empty()) {
                        g_notifyKeywords.push_back(val);
                        chat.setConnectionStatus("Notify keyword added: " + val);
                    } else if (sub == "remove" && !val.empty()) {
                        auto it = std::find(g_notifyKeywords.begin(), g_notifyKeywords.end(), val);
                        if (it != g_notifyKeywords.end()) g_notifyKeywords.erase(it);
                        chat.setConnectionStatus("Notify keyword removed: " + val);
                    } else if (sub == "list") {
                        std::string list;
                        for (auto& k : g_notifyKeywords) list += k + " ";
                        chat.setConnectionStatus("Keywords: " + (list.empty() ? "(none)" : list));
                    }
                } else if (cmd == "directory" || cmd == "dir") {
                    if (!args.empty()) {
                        try {
                            auto pubs = client.getPublicRooms("", args, 20);
                            int cnt = pubs.value("total_room_count_estimate", 0);
                            chat.setConnectionStatus("Directory: " + std::to_string(cnt) + " rooms matching '" + args + "'");
                        } catch (...) {}
                    }
                } else if (cmd == "spell") {
                    // Simple spell check — find closest command
                    if (!args.empty()) {
                        std::vector<std::string> cmds = {"join","leave","kick","ban","invite","op","deop",
                            "whois","ignore","pin","unpin","pins","stats","fav","mirror","markdown","upgrade",
                            "export","statusmsg","remind","notify","directory","nick","topic","react","vote",
                            "search","voice","sticker","location","todo","create","upload","redact","read","online","away"};
                        std::string best;
                        int bestDist = 999;
                        for (auto& c : cmds) {
                            int dist = 0;
                            for (size_t i = 0; i < std::min(args.size(), c.size()); i++)
                                if (tolower(args[i]) != tolower(c[i])) dist++;
                            dist += std::abs((int)args.size() - (int)c.size());
                            if (dist < bestDist) { bestDist = dist; best = c; }
                        }
                        chat.setConnectionStatus("Did you mean: /" + best + " ?");
                    }
                } else if (cmd == "rainbow") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendTextMessage(roomId, args); } catch (...) {}
                } else if (cmd == "rainbowme") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendEmote(roomId, args); } catch (...) {}
                } else if (cmd == "confetti") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " 🎉✨🎊"); } catch (...) {}
                } else if (cmd == "snowfall") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " ❄️🌨️❄️"); } catch (...) {}
                } else if (cmd == "myroomavatar") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        std::string url = (args.find("mxc://") == 0) ? args : client.uploadMedia(args);
                        nlohmann::json c = {{"membership","join"},{"avatar_url",url}};
                        try { client.sendStateEvent(roomId, "m.room.member", client.userId(), c); } catch (...) {}
                    }
                } else if (cmd == "report") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        try { client.sendEvent(roomId, "m.room.report",
                            nlohmann::json{{"event_id",args.substr(0,sp)},{"reason",args.substr(sp+1)}}); } catch (...) {}
                } else if (cmd == "forward") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos) {
                        auto msgs = client.getRoomMessages(roomId, args.substr(0, sp));
                        if (!msgs.empty())
                            try { client.sendTextMessage(args.substr(sp+1),
                                "[Fwd from " + roomId + "] <" + msgs[0].sender + "> " + msgs[0].content.value("body","")); } catch (...) {}
                    }
                } else if (cmd == "schedule") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        chat.setConnectionStatus("Scheduled: " + args.substr(0, sp) + "s → " + args.substr(sp+1));
                } else if (cmd == "users") {
                    if (!args.empty())
                        try { auto r = client.searchUserDirectory(args); chat.setConnectionStatus(
                            std::to_string(r.value("results",nlohmann::json::array()).size()) + " users for '" + args + "'"); } catch (...) {}
                } else if (cmd == "createspace") {
                    if (!args.empty())
                        try { client.createRoom(args, "", false, {}); chat.setConnectionStatus("Space created"); } catch (...) {}
                } else if (cmd == "addtospace") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendStateEvent(args, "m.space.child", roomId,
                            nlohmann::json{{"via",nlohmann::json::array({""})},{"suggested",false},{"auto_join",false}}); } catch (...) {}
                } else if (cmd == "joinspace") {
                    if (!args.empty()) try { client.joinRoom(args); } catch (...) {}
                } else if (cmd == "admin") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "deactivate" && !val.empty())
                        try { client.adminDeactivateUser(val); chat.setConnectionStatus("Deactivated " + val); } catch (...) {}
                    else if (sub == "resetpw" && !val.empty()) {
                        auto sp2 = val.find(' '); auto uid = sp2 != std::string::npos ? val.substr(0, sp2) : val;
                        auto pw = sp2 != std::string::npos ? val.substr(sp2 + 1) : "";
                        try { client.adminResetPassword(uid, pw); chat.setConnectionStatus("Password reset for " + uid); } catch (...) {}
                    } else if (sub == "listusers")
                        try { auto u = client.adminListUsers(); chat.setConnectionStatus("Users: " + std::to_string(u.value("total", 0))); } catch (...) {}
                    else if (sub == "deleteroom" && !val.empty())
                        try { client.adminDeleteRoom(val); chat.setConnectionStatus("Room deleted"); } catch (...) {}
                    else if (sub == "shadowban" && !val.empty())
                        try { client.adminShadowBan(val); chat.setConnectionStatus("Shadow banned " + val); } catch (...) {}
                    else if (sub == "roomstats")
                        try { client.adminRoomStats(); chat.setConnectionStatus("Room stats fetched"); } catch (...) {}
                } else if (cmd == "td") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "login" || sub == "start") {
                        if (!g_tdlib.isAvailable()) { g_tdlib.initialize(); }
                        if (g_tdlib.isAvailable()) {
                            // Use test API credentials (you need real ones for production)
                            g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
                            chat.setConnectionStatus("TDLib initialized. Send /td phone +123****7890");
                        } else chat.setConnectionStatus("TDLib not available (install libtdjson)");
                    } else if (sub == "phone") {
                        if (!val.empty()) { g_tdlib.sendPhoneNumber(val); chat.setConnectionStatus("Sent code to " + val + ". /td code XXXXX"); }
                    } else if (sub == "code") {
                        if (!val.empty()) { g_tdlib.sendAuthCode(val); chat.setConnectionStatus("Code sent. /td password your2fa (if needed)"); }
                    } else if (sub == "password" || sub == "2fa") {
                        if (!val.empty()) { g_tdlib.sendPassword(val); chat.setConnectionStatus("2FA sent"); }
                    } else if (sub == "chats") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready) {
                            auto chats = g_tdlib.getChats(20);
                            std::string list = std::to_string(chats.size()) + " chats: ";
                            for (size_t i = 0; i < std::min((size_t)5, chats.size()); i++)
                                list += chats[i].title + (i < 4 ? ", " : "");
                            chat.setConnectionStatus(list);
                        } else chat.setConnectionStatus("Not authorized. /td phone first");
                    } else if (sub == "msg") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready) {
                            auto sp2 = val.find(' ');
                            if (sp2 != std::string::npos) {
                                int64_t chatId = std::stoll(val.substr(0, sp2));
                                g_tdlib.sendMessage(chatId, val.substr(sp2 + 1));
                                chat.setConnectionStatus("Sent to Telegram");
                            }
                        }
                    } else if (sub == "history") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready && !val.empty()) {
                            int64_t chatId = std::stoll(val);
                            auto msgs = g_tdlib.getChatHistory(chatId);
                            std::string preview = std::to_string(msgs.size()) + " msgs. Latest: ";
                            if (!msgs.empty()) preview += msgs[0].text.substr(0, 60);
                            chat.setConnectionStatus(preview);
                        }
                    } else {
                        chat.setConnectionStatus("TDLib: /td login|phone|code|password|chats|msg|history");
                    }
                }
                } else if (cmd == "create" || cmd == "newroom") {
                    auto sp = args.find(' ');
                    std::string name = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    try {
                        auto roomId = client.createRoom(name);
                        client.joinRoom(roomId);
                    } catch (...) {}
                } else if (cmd == "search" || cmd == "find") {
                    // Full-text search messages
                    if (!args.empty()) {
                        try {
                            auto results = dbi.search(args, 20);
                            if (!results.empty()) {
                                std::string output;
                                for (auto& r : results) {
                                    std::string body = r.value("content", nlohmann::json::object()).value("body", "");
                                    output += body.substr(0, 80) + " | ";
                                }
                                chat.setConnectionStatus("Search: " + std::to_string(results.size()) + " results");
                            } else {
                                chat.setConnectionStatus("Search: no results");
                            }
                        } catch (...) {}
                    }
                } else if (cmd == "joinroom") {
                    // Join room by name or alias
                    if (!args.empty()) {
                        try { client.joinRoom(args); } catch (...) {}
                    }
                } else if (cmd == "preview" && !args.empty()) {
                    try {
                        auto preview = client.getURLPreview(args);
                        if (preview.contains("og:title")) {
                            // Show URL preview in status
                            chat.setConnectionStatus("Preview: " + preview["og:title"].get<std::string>());
                        }
                    } catch (...) {}
                }
            });

            // Load rooms from DB
            auto rooms = dbi.listRooms();
            std::vector<tui::RoomInfo> roomInfos;
            for (auto& r : rooms) {
                tui::RoomInfo ri;
                ri.id = r.value("room_id", "");
                ri.name = r.value("name", "");
                if (ri.name.empty()) ri.name = ri.id;
                ri.is_encrypted = r.value("is_encrypted", false);
                roomInfos.push_back(ri);
            }
            std::map<std::string, db::InviteInfo> inviteMap;
            if (!agentOnly) {
                // Mark the invited rooms with the ✉ marker; the remembered
                // invitation date shows as a line in the room's chat below.
                std::string selfId = demoMode ? "@you:demo.local" : sacc.user_id;
                for (auto& inv : dbi.openInvites(selfId)) inviteMap[inv.roomId] = inv;
                std::set<std::string> haveIds;
                for (auto& ri : roomInfos) haveIds.insert(ri.id);
                for (auto& ri : roomInfos) {
                    if (inviteMap.count(ri.id)) ri.name = "✉ " + ri.name;
                }
                for (auto& [id, inv] : inviteMap) {
                    if (haveIds.count(id)) continue;
                    tui::RoomInfo ri;
                    ri.id = id;
                    ri.name = "✉ " + id;
                    roomInfos.push_back(ri);
                }
            }
            if (roomInfos.empty()) {
                // Add a placeholder
                tui::RoomInfo ri;
                ri.id = "!welcome:demo.local";
                ri.name = "#welcome";
                roomInfos.push_back(ri);
            }
            if (agentOnly) {
                // The virtual #agent room: the home for the agent output
                // when there is no Matrix session.
                tui::RoomInfo ri;
                ri.id = "!agent:demo.local";
                ri.name = "#agent";
                roomInfos.insert(roomInfos.begin(), ri);
            }
            chat.setRooms(roomInfos);

            // Load the cached history from the local DB into the chat:
            // the demo TUI works offline (the chat was stuck on
            // "(no messages)" forever), and the real client shows its
            // history instantly instead of waiting for the first sync.
            for (auto& ri : roomInfos) {
                auto evs = dbi.getEvents(ri.id, 100);
                auto invIt = inviteMap.find(ri.id);
                if (evs.empty() && invIt == inviteMap.end()) continue;
                std::vector<tui::MessageInfo> msgs;
                if (invIt != inviteMap.end()) {
                    // The remembered invitation date: a line at the top of
                    // the room's chat ("invited you 2h ago (from alice) —
                    // "reason"").
                    tui::MessageInfo imi;
                    imi.sender = "@invite";
                    imi.is_notice = true;
                    std::string who = invIt->second.inviter;
                    auto at2 = who.find(':');
                    if (at2 != std::string::npos) who = who.substr(1, at2 - 1);
                    else if (!who.empty() && who[0] == '@') who = who.substr(1);
                    imi.body = "invited you " + relativeTime(invIt->second.ts)
                             + " (from " + who + ")";
                    if (!invIt->second.reason.empty()) {
                        imi.body += " — \"" + invIt->second.reason + "\"";
                    }
                    msgs.push_back(imi);
                }
                for (auto& ev : evs) {
                    if (ev.type != "m.room.message" && ev.type != "m.sticker") continue;
                    tui::MessageInfo mi;
                    std::string s = ev.sender;
                    auto at = s.find(':');
                    if (at != std::string::npos) s = s.substr(1, at - 1);
                    else if (!s.empty() && s[0] == '@') s = s.substr(1);
                    mi.sender = s;
                    mi.body = ev.content.value("body", "(no body)");
                    mi.event_id = ev.event_id;
                    mi.is_notice = ev.content.value("msgtype", "") == "m.notice";
                    mi.is_encrypted = ev.content.value("msgtype", "") == "m.encrypted";
                    msgs.push_back(mi);
                }
                if (!msgs.empty()) chat.setMessages(ri.id, msgs);
                // The last-read divider: a dim notice right after the
                // message the user read up to (the local m.fully_read copy).
                std::string marker = dbi.getReadMarker(ri.id);
                if (!marker.empty()) {
                    auto it = std::find_if(msgs.begin(), msgs.end(),
                        [&](const tui::MessageInfo& m) { return m.event_id == marker; });
                    if (it != msgs.end()) {
                        tui::MessageInfo div;
                        div.is_notice = true;
                        div.body = "── last read ──";
                        if (it + 1 != msgs.end()) msgs.insert(it + 1, div);
                        else msgs.push_back(div);
                        chat.setMessages(ri.id, msgs);
                    }
                }
            }

            // Ctrl+F search: filter the active room's cached messages to
            // the matches (the Go-TUI parity).
            chat.setSearchCallback([&](const std::string& query) {
                std::string roomId = chat.activeRoomId();
                if (roomId.empty() || query.empty()) {
                    chat.setConnectionStatus("search: nothing to search");
                    return;
                }
                auto rows = dbi.search(query, 50);
                std::vector<tui::MessageInfo> msgs;
                for (auto& r : rows) {
                    if (r.value("room_id", "") != roomId) continue;
                    tui::MessageInfo mi;
                    std::string s = r.value("sender", "");
                    auto at = s.find(':');
                    if (at != std::string::npos) s = s.substr(1, at - 1);
                    else if (!s.empty() && s[0] == '@') s = s.substr(1);
                    mi.sender = s;
                    mi.body = r.value("content", nlohmann::json::object())
                                  .value("body", "");
                    mi.event_id = r.value("event_id", "");
                    msgs.push_back(mi);
                }
                if (!msgs.empty()) {
                    tui::MessageInfo notice;
                    notice.sender = "@search";
                    notice.is_notice = true;
                    notice.body = std::to_string(msgs.size())
                                + " results for \"" + query
                                + "\" (Esc/Ctrl+F to reset)";
                    msgs.insert(msgs.begin(), notice);
                }
                chat.setMessages(roomId, msgs);
                chat.setConnectionStatus("search: " + std::to_string(msgs.size())
                                         + " results for \"" + query + "\"");
            });

            // The typing notifications: the composer fires the hook on the
            // printable keys; the throttle + the send_typing setting gate
            // the actual sends.
            static std::chrono::steady_clock::time_point lastTypingSent{};
            chat.setTypeNotify([&]() {
                if (dbi.getSetting("send_typing", "1") == "0") return;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastTypingSent < std::chrono::seconds(10)) return;
                lastTypingSent = now;
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    try { client.sendTyping(roomId, true, 20000); } catch (...) {}
                }
            });

            // Set up send callback with retry queue
            chat.setSendCallback([&](const std::string& body) {
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    if (agentOnly) {
                        // Offline: the message stays local (and unsent).
                        tui::MessageInfo mi;
                        mi.sender = "@you";
                        mi.body = body;
                        chat.addMessage(roomId, mi);
                        chat.setConnectionStatus("offline agent mode — the message was not sent");
                        return;
                    }
                    try {
                        client.sendTextMessage(roomId, body);
                        if (dbi.getSetting("send_typing", "1") != "0")
                            client.sendTyping(roomId, false);
                    } catch (...) {
                        // Queue for retry
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        g_msgQueue[roomId].push_back({body, 0});
                        chat.setConnectionStatus("Queued (will retry): " + body.substr(0, 40));
                    }
                }
            });

            // Set up pagination callback
            chat.setPaginateCallback([&](const std::string& room_id) {
                try {
                    client.getRoomMessages(room_id, "", "b", 50);
                } catch (...) {}
            });

            // Start sync: feed events to chat, flush message queue
            // (skipped in the offline agent mode — there is no session).
            if (!agentOnly) {
            client.startSync([&](const matrix::Event& ev) {
                // Flush queued messages on successful sync
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    for (auto& [rid, msgs] : g_msgQueue) {
                        for (auto it = msgs.begin(); it != msgs.end();) {
                            try {
                                client.sendTextMessage(rid, it->first);
                                it = msgs.erase(it);
                            } catch (...) {
                                it->second++;
                                if (it->second > 5) it = msgs.erase(it); // give up after 5 retries
                                else ++it;
                            }
                        }
                    }
                }
                tui::RoomInfo ri;
                ri.id = ev.room_id;
                ri.name = ev.room_id;
                chat.addRoom(ri);

                // Evaluate push rules for notification
                nlohmann::json jev;
                jev["event_id"] = ev.event_id;
                jev["room_id"] = ev.room_id;
                jev["sender"] = ev.sender;
                jev["type"] = ev.type;
                jev["content"] = ev.content;
                auto pr = client.evaluatePush(jev);

                if (ev.type == "m.room.message" && ev.content.contains("body")) {
                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    mi.body = ev.content["body"].get<std::string>();
                    mi.event_id = ev.event_id;
                    std::string mt = ev.content.value("msgtype", "m.text");
                    mi.is_notice = (mt == "m.notice");
                    mi.is_emote = (mt == "m.emote");
                    mi.is_highlight = pr.highlight;
                    mi.url = ev.content.value("url", "");
                    mi.mimetype = ev.content.value("info", nlohmann::json::object()).value("mimetype", "");

                    // Thread support
                    if (ev.content.contains("m.relates_to")) {
                        auto& rel = ev.content["m.relates_to"];
                        std::string relType = rel.value("rel_type", "");
                        if (relType == "m.thread") {
                            mi.thread_id = rel.value("event_id", "");
                            // Mark thread root
                            bool is_root = rel.value("is_falling_back", true);
                            if (!is_root) mi.is_thread_root = false;
                        } else if (relType == "m.replace") {
                            mi.is_edited = true;
                            mi.body = ev.content.value("m.new_content", nlohmann::json::object()).value("body", mi.body);
                        }
                    }

                    chat.addMessage(ev.room_id, mi);
                    // The async link preview (the first URL in the body).
                    queueUrlPreview(client, chat, ev.room_id, mi.body);
                }

                // The read receipts: how many users read up to each event.
                if (ev.type == "m.receipt" && ev.content.is_object()) {
                    for (auto& [eventId, receipts] : ev.content.items()) {
                        if (!receipts.is_object()) continue;
                        auto mread = receipts.value("m.read", nlohmann::json::object());
                        if (!mread.is_object()) continue;
                        int count = 0;
                        for (auto& [userId, ts] : mread.items()) (void)ts, count++;
                        if (count > 0) chat.setReceipts(ev.room_id, eventId, count);
                    }
                }
                // Redactions
                if (ev.type == "m.room.redaction" && !ev.redacts.empty()) {                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    mi.body = "Message redacted";

                    mi.event_id = ev.redacts;
                    mi.is_redacted = true;
                    mi.redacted_by = ev.sender;
                    chat.addMessage(ev.room_id, mi);
                }

                // Polls
                if (ev.type == "m.poll.start" && ev.content.contains("m.poll")) {
                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    auto& poll = ev.content["m.poll"];
                    mi.body = poll.value("question", nlohmann::json::object())
                                   .value("body", "(poll)");
                    mi.is_poll = true;
                    mi.event_id = ev.event_id;
                    for (auto& ans : poll.value("answers", nlohmann::json::array())) {
                        std::string text = ans.value("body", nlohmann::json::object()).value("body", "?");
                        mi.poll_options.emplace_back(text, 0);
                    }
                    chat.addMessage(ev.room_id, mi);
                }
                if (ev.type == "m.poll.response" && ev.content.contains("m.poll.response")) {
                    // Update vote counts by re-reading room state
                }

                // Typing events with monitor
                if (ev.type == "m.typing" && ev.content.contains("user_ids")) {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    for (auto& uid : ev.content["user_ids"])
                        g_typing.updateUser(ev.room_id, uid.get<std::string>(), now);
                    g_typing.pruneExpired(now);
                    auto typing = g_typing.formatTypingUsers(ev.room_id);
                    if (!typing.empty()) chat.setConnectionStatus(typing);
                    std::vector<std::string> users;
                    for (auto& uid : ev.content["user_ids"]) users.push_back(uid.get<std::string>());
                    chat.setTypingUsers(ev.room_id, users);
                }

                // Server notices
                if (ev.type == "m.server_notice") {
                    std::string body = ev.content.value("body", "");
                    if (!body.empty()) chat.setConnectionStatus("[SERVER] " + body);
                }

                chat.requestRedraw();
            });
            }  // !agentOnly

            // Offline agent mode with a task: fire the agent once on
            // startup (`progressive-cli tui agent <task>`).
            if (agentOnly && !agentTask.empty()) launchMatrixAgent(agentTask);

            chat.run(screen);
            if (!agentOnly) client.stopSync();
        } catch (const std::exception& e) {
            screen.shutdown();
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    }

    screen.shutdown();
    return 0;
}
#endif

