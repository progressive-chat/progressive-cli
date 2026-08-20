# progressive-cli

**progressive-cli** is a **Matrix chat client and coding agent designed to
work in the terminal** — built in C++23. The command line is the primary
interface; the ncurses TUI, the ASCII client and the REST API server are
the other surfaces. Minimal dependencies (POSIX sockets + OpenSSL),
maximum portability.

The project started its life as a gomuks fork (2024) and was rewritten in
C++ from the ground up — the old Go implementation is retired (the
`go-legacy` tag keeps it for the history).

> **Note:** This is an independent project in the Progressive Chat
> ecosystem. The Android client lives in
> [progressive-android](https://github.com/progressive-chat/progressive-android),
> the shared Qt-free core in
> [progressive-core](https://github.com/progressive-chat/progressive-core).
>
> The docs live in [`docs/`](docs/) — the GitHub Pages sources. The
> [feature status table](docs/status.html) is stamped with the
> version/commit/build it was generated from, and every feature carries a
> code anchor (file + line) verified by `docs/check.mjs` in the CI.

## Highlights

- **The full CLI client** — rooms, messages, threads, polls, reactions,
  E2EE (Olm/Megolm, the SAS device verification, key backup), spaces,
  the per-room read receipts, the last-read markers, the media send
  presets (`attach` + `sendpreset original|compact|full`)
- **Multi-format REST API** — JSON, plain text, Markdown, Gemtext
  (Gemini protocol), or HTML (`matrixcli serve`)
- **Onion/I2P/Yggdrasil transport** — SOCKS5 for Tor, HTTP for I2P, the
  native Yggdrasil IPv6 — the connection is chosen at the login
- **The LLM conversations + the agents** — the streaming chat with the
  persistent sessions (`llm`, `llm chat`, `llm continue`, `llm resume`),
  the tool access (`--tools`: the filesystem/shell, the PTY `process`
  tool for the interactive debugging, subagents, MCP), the Matrix-tools
  agent and the coding agent
- **The VoIP signaling** — `matrixcli call` (the m.call.* state machine;
  the WebRTC media plane is the next stage)
- **The tests + the CI** — the unit tests for the LLM plumbing, the
  sessions and the db, the agent-loop integration test, the CI with the
  ASan/UBSan job

## Project structure

```
matrixcli/
└── cppcli/          # The C++23 client (the whole thing)
    ├── lib/http/    #   POSIX + OpenSSL HTTP client, SOCKS5/HTTP proxy
    ├── lib/matrix/  #   Matrix protocol (login, sync, send, events)
    ├── lib/         #   E2EE/sync core: fetched from progressive-chat/
    │                #   progressive-core (FetchContent, matrixcli_ecore
    │                #   alias; prebuilt artifact with source fallback)
    ├── lib/api/     #   HTTP server, content negotiation, routing
    ├── lib/formats/ #   Format renderers (JSON, text, MD, gemtext, HTML)
    └── lib/tui/     #   ncurses terminal UI
```

The dependencies (libolm 3.2.16, nlohmann/json, simdjson) are fetched at
the build time via FetchContent and cached in the build tree — the repo
itself stays dependency-free.

### Terminal TUI login

The login screen supports connection types:

| Type | Description | Default proxy |
|------|-------------|---------------|
| Direct | No proxy (default) | — |
| Tor | Route through Tor daemon | SOCKS5 `127.0.0.1:9050` |
| I2P | Route through I2P router | HTTP `127.0.0.1:4444` |
| Yggdrasil | Mesh network (200::/7, .ygg domains) | URL rewrite |
| Custom | User-specified proxy | Configurable host/port/credentials |

### Demo mode (`matrixcli demo`)

An offline demo (no Matrix account needed): an interactive REPL against the
demo database, one-shot `cli` mode, the ASCII client interface, the ncurses
TUI, plus the markdown rendering and poll vote showcases:

```bash
# Interactive demo session (type help at the prompt)
matrixcli demo

# Populate the demo DB and exit (then run the one-shot commands)
matrixcli demo cli

# The markdown rendering demo: what the chat view does with a message
matrixcli demo markdown

# The poll vote demo: 15 votings in 10 rooms, pick one and vote
matrixcli demo vote
```

The same vote flow reads any cache: with a real account the one-shot
`matrixcli vote <room> <poll_event_id> <answer>` sends the vote to the
homeserver.

### Markdown rendering in the terminal

`demo markdown` renders the same sample the chat view uses. Supported:
**bold**, *italic*, `` `inline code` ``, `[text](url)` links, `#`/`##`/`###`
headers, bullets, `- [x]`/`- [ ]` checkboxes, numbered lists, `> ` quotes
and fenced code blocks. The terminal realities (verified against real
terminals and terminal source code):

- **No big text.** Terminals draw a fixed-size cell grid; no font-size
  escape exists, so headers render as bold.
- **Bold/italic need font faces.** If the terminal font has no bold or
  italic face (common on phone terminals), the SGR codes (1, 3) still
  parse but the glyphs look identical to normal text.
- **Links are OSC 8 hyperlinks**, clickable in terminals that implement
  OSC 8 (kitty, alacritty, foot, GNOME Terminal, Windows Terminal,
  Konsole ≥ 21.08, ...). The URL is also printed dim, so the link
  survives terminals without OSC 8. Old emulators (VTE < 0.46.2,
  `screen`, tmux < 3.0) may strip or garble the sequences.
- **Konsole specifics** (verified against Konsole source): OSC 8
  interactions are opt-in per profile — *Settings → Configure Konsole →
  Edit Profile → Mouse tab → tick "Allow escape sequences for links"*
  (a security warning appears; leave "Allowed link formats" at its
  default). Optionally tick *"Open files/links by direct click"* to
  open with a plain tap/click instead of Ctrl+Click. The corner URL
  preview appears once enabled. Opening hands the URL to KIO, which
  launches your desktop default browser — a dangling default (e.g. an
  uninstalled browser in `mimeapps`) silently kills the click: `gio
  mime x-scheme-handler/https` shows what KIO would use, and
  `xdg-mime default <browser>.desktop x-scheme-handler/https` fixes it.
  The right-click *Open Link* menu item exists only for bare typed
  URLs; for OSC 8 links it is still an upstream Konsole gap
  (KDE bug 520743).

### Native desktop notifications

New messages can pop up as native desktop notifications (KDE Plasma,
GNOME, ...): the program talks to the desktop's notification daemon via
`libnotify` (`notify-send`), with a direct `qdbus6`/`qdbus` D-Bus call as
the fallback (Plasma runs the same `org.freedesktop.Notifications`
service in its tray), and always rings the terminal bell.

```bash
# Test the notification (should appear as a popup in your desktop)
matrixcli notify test "hello, tray"
matrixcli notify test                  # the default test text
matrixcli notify last                  # re-send the newest unread message
matrixcli notify on|off                # the persisted switch (default on)
```

The `ui` command announces, on `refresh`, the unread notifications that
arrived since the last refresh (tracked via `notify_seen_id`). The
toggle is also in the `settings` output. If nothing pops up, check the
backend: `matrixcli notify test` reports when `notify-send`/`qdbus6`
is missing (some minimal KDE installs lack `libnotify`).

**Another user's session.** When the matrix client runs outside the
desktop session (SSH, a service, a different Linux user) it can still
pop up notifications in the session that owns the desktop: run the
forwarding service *there* (it just listens on a TCP port, loopback by
default) and point the client at it:

```bash
# In the desktop-owning session (e.g. as the desktop user):
matrixcli notify daemon --port 27430          # Ctrl+C stops it
# [sudo -u <desktop-user> ... if that session belongs to another user]

# From the matrix client (any machine/user that can reach the port):
matrixcli notify host 127.0.0.1:27430         # persist; notify host off resets
matrixcli notify test "hello, tray"           # now routed through the daemon
```

**Optional: autostart the daemon as a systemd user service.** The Arch
package ships `progressive-cli-notify.service` (installed but *not*
enabled). As the desktop-owning user, opt in with:

```bash
systemctl --user enable --now progressive-cli-notify   # starts with each login
systemctl --user disable --now progressive-cli-notify  # opt out any time
```

On non-Arch systems copy `packaging/arch/progressive-cli-notify.service`
to `~/.config/systemd/user/` first, or just keep running the daemon
manually as above. The service is harmless either way: it only listens
on loopback and only forwards when a client sets `notify host`.

The daemon forwards each notification to *its* session's daemon
(`notify-send`, with the qdbus6 fallback), so the popup appears in that
user's Plasma tray. Wire format is one line per notification
(`title<TAB>body`), so `echo -e "title\tbody" | nc 127.0.0.1 27430`
works too. Keep the daemon bound to loopback unless you need remote
users — there is no authentication on the port.

### REST API (`matrixcli serve`)

The server exposes a format-aware REST API under `/api/` (default port
8080, `--port` to change). Demo mode (`matrixcli serve --demo`) serves the
same API with synthetic data and no account:

```bash
# Start the API server (demo mode, no account needed)
matrixcli serve --demo --port=8080

# Get client status
curl "http://localhost:8080/api/status?format=json"

# List rooms as Markdown
curl "http://localhost:8080/api/rooms?format=markdown"

# Get room messages as plain text
curl "http://localhost:8080/api/rooms/!roomid:server/messages?format=text&limit=20"

# Get room messages as HTML
curl "http://localhost:8080/api/rooms/!roomid:server/messages?format=html"

# Get room messages as Gemtext
curl "http://localhost:8080/api/rooms/!roomid:server/messages?format=gemini"
```

Available formats: `json` (default), `text`, `markdown`, `gemini`, `html`.
The format can also be selected via the `Accept:` header:

```bash
curl -H "Accept: text/markdown" http://localhost:8080/api/status
```

The full endpoint list is returned by `/api/status` itself.

## C++ build (cppcli/)

Requires: CMake 3.20+, a C++23 compiler, OpenSSL, ncurses. The rest
(libolm, nlohmann/json, simdjson, the E2EE core) is fetched by
FetchContent.

```bash
cd cppcli
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./matrixcli serve   # Start API server
./matrixcli login   # Interactive login
./matrixcli tui     # Terminal UI
./matrixcli call    # VoIP signaling: call <@user> | answer | hangup | status | wait
```

## The LLM / agent CLI

The client ships with an LLM conversational interface and an agentic
coding engine (the same engine backs the ASCII UI and the TUI):

```bash
matrixcli llm "question"                 # one-shot (streams on a TTY)
matrixcli llm continue "follow-up"       # the next turn (stdin works too)
matrixcli llm chat                       # the interactive multi-turn chat
matrixcli llm sessions                   # the saved conversations (+previews)
matrixcli llm resume 2 "..."             # switch + continue by number
matrixcli llm --session work "..."       # a named conversation
matrixcli llm --fresh "..."              # archive + start anew

matrixcli llm "..." --tools              # the agent with the tool access
matrixcli llm chat --tools               # interactive + tools per turn
matrixcli agent-code "fix the build"     # the coding agent (opencode-style)
matrixcli agent "summarize #general"     # the Matrix-tools agent
```

- The conversations persist in `~/.local/share/matrixcli/sessions/`
  (XDG); `--fresh` archives, nothing is ever deleted (`sessions rm`
  moves to the trash).
- Streaming (SSE, chunked transfer), the usage/price/context meta
  (`--rich`), the light-grey thinking by default (`--no-reasoning`
  hides it), markdown rendering (`--markdown`), JSON output (`--json`).
- The provider config: `~/.config/matrixcli/agent.json` (presets:
  openai, anthropic, deepseek, qwen, openrouter, groq, fireworks, mimo,
  ollama, lmstudio) or the interactive first-run wizard
  (`matrixcli tui agent`).
- The `--tools` engine: filesystem/shell with the trust policy
  (allow/ask/deny, `y/N/a/A` prompts, the hardline dangerous-command
  blocks), subagents, MCP, plan mode, goals, cron, and the interactive
  `process` tool (a PTY per process: start/send/poll/wait/kill — for
  gdb/pdb-style debugging).

### The ASCII UI settings

`matrixcli demo ui --static` draws one frame and exits (non-interactive,
pipe-friendly). In the interactive UI the `settings` screen lists every
toggle:

- `invites on|off` — the "📥 N (invites)" counter in the rooms header
- `notifications on|off` — the bottom-right corner: recent @-pings of the
  logged-in account plus the read receipts of 100%-monitored rooms
- `monitor <room> <0-100|off>` — the room's monitoring level; receipts
  become notifications at 100%
- `spaces [--json]` — list the spaces in the cache (one-shot command)

## Features

- [x] The Matrix client with the terminal TUI (markdown, threads, polls,
      reactions, the Ctrl+F search, the link previews, the read receipts)
- [x] The ASCII client and the multi-format REST API
- [x] Tor / I2P / Yggdrasil proxy support
- [x] Enhanced login: well-known discovery, SSO URL, token auth
- [x] End-to-end encryption (Olm + Megolm, the SAS device verification,
      key backup)
- [x] The SQLite-backed offline event store
- [x] The LLM conversations, the streaming, the tools, the agents
- [x] The per-room read receipts and the last-read markers
- [~] The VoIP signaling (the m.call.* state machine; the WebRTC media
      plane is the next stage) — `matrixcli call`

## License

Licensed under the **GNU Affero General Public License v3.0** (AGPLv3).
The full text: [LICENSE](LICENSE).

```
Copyright (C) 2024-2025 Tulir Asokan (the gomuks portions)
Copyright (C) 2026 Progressive Matrix Client contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.
```

## Links

- Website: [progressive.chat](https://progressive.chat)
- Matrix room: [#community:progressive.chat](https://matrix.to/#/#community:progressive.chat)

## Related projects

- [progressive-android](https://github.com/progressive-chat/progressive-android) —
  the Android client
- [progressive-core](https://github.com/progressive-chat/progressive-core) —
  the shared Qt-free Matrix core (fetched by this build)
- [gomuks](https://github.com/tulir/gomuks) — the Go client this project
  started from (the `go-legacy` tag here)
