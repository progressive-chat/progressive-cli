# matrixcli

A **hard fork** of [gomuks](https://github.com/tulir/gomuks) — a Matrix client
with a terminal TUI, multi-format REST API, and a ground-up C++ rewrite in
progress. Originally based on the Progressive Android Matrix client's native
C++ acceleration layer.

## Why a fork?

gomuks is an excellent Go Matrix client. This fork extends it with:

- **Multi-format REST API** — query rooms, messages, and client state in
  JSON, plain text, Markdown, Gemtext (Gemini protocol), or HTML
- **Onion/I2P/Yggdrasil transport** — built-in proxy support for anonymous
  and mesh networks (SOCKS5 for Tor, HTTP for I2P, native Yggdrasil IPv6)
- **C++ rewrite** — a parallel C++20 codebase (`cppcli/`) with POSIX socket
  HTTP client, OpenSSL TLS, libolm E2EE, and ncurses TUI — targeting
  minimal dependencies and maximum portability
- **Enhanced login** — server discovery via `.well-known`, SSO URL builder,
  token login, proxy-aware authentication flow

## Project structure

```
matrixclient/
├── gomuks/          # Hard fork of gomuks (Go backend + TUI)
│   ├── pkg/hicli/   #   Enhanced login, proxy, sync, E2EE
│   ├── pkg/hicli/api/  # Multi-format API renderers
│   ├── pkg/gomuks/  #   HTTP server, REST API endpoints
│   └── tui/         #   Terminal TUI with proxy/network selector
├── cppcli/          # C++20 CLI rewrite (in progress)
│   ├── lib/http/    #   POSIX + OpenSSL HTTP client, SOCKS5/HTTP proxy
│   ├── lib/matrix/  #   Matrix protocol (login, sync, send, events)
│   ├── lib/e2ee/    #   libolm C++ wrappers (OlmAccount, Megolm, CryptoManager)
│   ├── lib/api/     #   HTTP server, content negotiation, routing
│   ├── lib/formats/ #   Format renderers (JSON, text, MD, gemtext, HTML)
│   └── lib/tui/     #   ncurses terminal UI
└── ROADMAP.md       # Full porting roadmap (in Russian)
```

## Quick start (Go / gomuks fork)

```bash
cd gomuks
go build ./cmd/gomuks
./gomuks
```

Then open `http://localhost:29325` in a browser, or use the terminal TUI:

```bash
cd gomuks
go build ./cmd/gomuks-terminal
./gomuks-terminal
```

### Terminal TUI login

The login screen supports connection types:

| Type | Description | Default proxy |
|------|-------------|---------------|
| Direct | No proxy (default) | — |
| Tor | Route through Tor daemon | SOCKS5 `127.0.0.1:9050` |
| I2P | Route through I2P router | HTTP `127.0.0.1:4444` |
| Yggdrasil | Mesh network (200::/7, .ygg domains) | URL rewrite |
| Custom | User-specified proxy | Configurable host/port/credentials |

### REST API

The backend exposes a format-aware REST API at `/_gomuks/api/v1/`:

```bash
# Get client status
curl http://localhost:29325/_gomuks/api/v1/status?format=json

# List rooms as Markdown
curl http://localhost:29325/_gomuks/api/v1/rooms?format=markdown

# Get room messages as plain text
curl "http://localhost:29325/_gomuks/api/v1/rooms/!roomid:server/messages?format=text&limit=20"

# Get room messages as HTML
curl "http://localhost:29325/_gomuks/api/v1/rooms/!roomid:server/messages?format=html"

# Get room info as Gemtext
curl http://localhost:29325/_gomuks/api/v1/rooms/!roomid:server?format=gemini
```

Available formats: `json` (default), `text`, `markdown`, `gemini`, `html`.

Format can also be selected via the `Accept:` header:
```bash
curl -H "Accept: text/markdown" http://localhost:29325/_gomuks/api/v1/status
```

## C++ build (cppcli/)

Requires: CMake 3.20+, C++20 compiler, OpenSSL, libolm, ncurses.

```bash
cd cppcli
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./matrixcli serve   # Start API server
./matrixcli login   # Interactive login
./matrixcli tui     # Terminal UI
```

## Features

- [x] Matrix client with terminal TUI
- [x] Multi-format REST API (JSON, text, Markdown, Gemtext, HTML)
- [x] Tor / I2P / Yggdrasil proxy support
- [x] Enhanced login: well-known discovery, SSO URL, token auth
- [x] End-to-end encryption (Olm + Megolm via mautrix-go / libolm)
- [x] SQLite-backed offline event store
- [x] C++ rewrite with POSIX sockets and OpenSSL (in progress)
- [ ] Full C++ TUI parity with Go TUI
- [ ] C++ E2EE device verification UI
- [ ] Voice/video calling (WebRTC)

## License

This project is a derivative work of gomuks, licensed under the
**GNU Affero General Public License v3.0** (AGPLv3).

```
Copyright (C) 2024-2025 Tulir Asokan (original gomuks)
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

Full text: [LICENSE](gomuks/LICENSE)

## Links

- Website: [progressive.chat](https://progressive.chat)
- Matrix room: [#community:progressive.chat](https://matrix.to/#/#community:progressive.chat)

## Upstream

- [gomuks](https://github.com/tulir/gomuks) — original Matrix client in Go
- [mautrix-go](https://github.com/mautrix/go) — Matrix Go SDK
- [libolm](https://gitlab.matrix.org/matrix-org/olm) — E2EE cryptographic library
