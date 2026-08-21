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

// populateDemoData part 1: the rooms, spaces, messages, polls,
// redactions, threads, power levels, the permalink pill.
#include "demo_tui.hpp"

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

    // The room NAME stays the meaningful title (e.g. "General discussion");
    // the TOPIC is a separate, longer admin-style blurb (what the room is
    // about, its history/vibe, and the rules) so it reads like a real room
    // topic and never equals the name.
    auto roomTopicForTitle = [](const std::string& title, const std::string& id) -> std::string {
        static const char* kDiscuss[] = {
            "news, questions and show-and-tell",
            "tips, help and war stories",
            "discussion, links and the occasional meme",
            "beginner questions and deep dives",
            "project updates and community chatter",
        };
        static const char* kVibe[] = {
            "A long-running room with a friendly, chatty crew",
            "One of the oldest rooms here, full of regulars",
            "A growing community of enthusiastic people",
            "A cozy corner for like-minded folks",
            "A busy hub with something happening every day",
        };
        unsigned h = 2166136261u;
        for (char c : id) { h ^= (unsigned char)c; h *= 16777619u; }
        const char* d = kDiscuss[h % 5];
        const char* v = kVibe[(h >> 4) % 5];
        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "%s. This is the place for %s about %s — browse the history to "
            "see what we've been up to. Room rules: stay on topic, be kind, "
            "no spam and no NSFW. New here? Say hi and introduce yourself!",
            v, d, title.c_str());
        return buf;
    };

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
        // The room NAME is the display title (the meaningful text, e.g.
        // "General discussion"), the ALIAS is its #handle — the ui can show
        // either (names/aliases). The TOPIC is a generated description kept
        // separate from the name, so `info` never shows name == topic. DMs
        // carry no topic and no alias, just their label ("Alice").
        bool isDm = std::string(r.id).find("!dm_") == 0;
        std::string title = isDm || std::string(r.topic).empty()
                                ? std::string(r.name)
                                : std::string(r.topic);
        j["name"] = title;
        j["canonical_alias"] = isDm ? "" : r.name;
        j["topic"] = isDm ? "" : roomTopicForTitle(title, r.id);
        j["member_count"] = r.members;
        // Room v12 (the m.room.create "creator"): @alice created every
        // demo room, so she is the owner (150) per Matrix 1.12 rules.
        j["creator"] = "@alice:demo.local";
        j["version"] = 12;
        // The DMs are encrypted by default, like Element.
        if (std::string(r.id).find("!dm_") == 0) j["is_encrypted"] = 1;
        dbi.upsertRoom(j, r.id);
        // A realistic m.room.power_levels state so `demo <room> power` shows
        // the room permissions offline: @alice is the creator/admin, @bob a
        // moderator; standard action thresholds.
        if (!isDm) {
            matrix::Event plEv;
            plEv.event_id = "$demo_pl_" + std::string(r.id);
            plEv.room_id = r.id;
            plEv.sender = "@alice:demo.local";
            plEv.type = "m.room.power_levels";
            plEv.state_key = "";
            plEv.content = nlohmann::json::object({
                {"ban", 50}, {"kick", 50}, {"redact", 50}, {"invite", 50},
                {"events_default", 0}, {"users_default", 0}, {"state_default", 50},
                {"users", nlohmann::json::object({
                    {"@alice:demo.local", 100}, {"@bob:demo.local", 50}})},
                {"notifications", nlohmann::json::object({
                    {"room", 50}, {"@room", 50}})}
            });
            dbi.insertEvent(plEv);
        }
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

    // A deep #general history: ~3 days of back-and-forth, inserted
    // OLDER than the seed above (so scrolling up reveals it) but newer
    // than the extras below. 15-minute cadence — enough rows to make
    // the chat window scroll a few screens past the visible height.
    {
        const char* genSenders[] = {"@alice", "@bob", "@carol", "@dave",
                                    "@erin", "@frank", "@grace"};
        const char* genBodies[] = {
            "Morning everyone, how's it going?",
            "Just finished the build, tests are green.",
            "Anyone up for a code review later?",
            "Coffee's brewing, let's talk APIs.",
            "The new sync endpoint landed — docs updated.",
            "I keep forgetting the flag order, ugh.",
            "Pushed a fix for the crash on startup.",
            "Meeting moved to 15:00, heads up.",
            "That bug only repros on the release build.",
            "Can someone check the merge request?",
            "Logs look clean now, nice work.",
            "Reminder: the demo is on Friday.",
            "What's everyone working on today?",
            "Haha, classic off-by-one.",
            "The spec says otherwise — let me quote it.",
            "Already rebased, ready when you are.",
        };
        constexpr int genCount = 288;             // 3 days at 15 min
        int64_t genTs = ts - dayMs;               // one day before the seed
        for (int i = 0; i < genCount; ++i) {
            genTs -= 900000;                      // 15 minutes between posts
            matrix::Event ev;
            ev.event_id = "$demo_hist_" + std::to_string(genTs);
            ev.room_id = "!general:demo.local";
            ev.sender = genSenders[i % 7];
            ev.type = "m.room.message";
            ev.content = {{"body", genBodies[i % 16]}, {"msgtype", "m.text"}};
            ev.origin_server_ts = genTs;
            dbi.insertEvent(ev);
        }
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
            {"!astronomy:demo.local", "@alice", "m.text", "Which planet is best for a new mission?"},
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
            {"!beer:demo.local", "@alice", "m.text", "Pilsner or IPA this Friday?"},
            {"!wine:demo.local", "@alice", "m.text", "Cork vs screwcap, discuss."},
            {"!boardgames:demo.local", "@alice", "m.text", "Which game for Friday night?"},
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

    // Power levels: alice owner (150), bob mod (50), a few custom levels
    // in the -1..99 band (charlie 20, carol 25, dave -1, erin 30, frank 90,
    // grace 10), and some of the formerly-plain members get theirs too
    // (kate 99, heidi 42, wendy 33, julia 7, trent 3, ivan 1, mallory -1,
    // alice:matrix.org 2); the rest stay plain members (0 / unlisted).
    {
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!general:demo.local"; pl.sender = "@alice";
        pl.type = "m.room.power_levels";
        pl.content = {{"users", {{"@alice", 150}, {"@bob", 50}, {"@charlie", 20},
                                 {"@carol", 25}, {"@dave", -1}, {"@erin", 30},
                                 {"@frank", 90}, {"@grace", 10}, {"@kate", 99},
                                 {"@heidi", 42}, {"@wendy:mozilla.org", 33},
                                 {"@julia", 7}, {"@trent:element.io", 3},
                                 {"@ivan", 1}, {"@mallory:matrix.org", -1},
                                 {"@alice:matrix.org", 2}}}};
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

    populateDemoDataExtras(dbi);
    std::cout << "Populated DB: " << (sizeof(rooms)/sizeof(rooms[0])) << " rooms, "
              << (sizeof(msgs)/sizeof(msgs[0])) + 2 + 4 << " messages (incl. a thread)." << std::endl;
    std::cout << "Try:  progressive-cli rooms | progressive-cli view #general | progressive-cli view #dev" << std::endl;
    return 0;
}
