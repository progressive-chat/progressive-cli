# progressive-cli (cppcli/)

A C++23 Matrix CLI client with TUI and HTTP API server.

## Build

```bash
cmake -S . -B /tmp/opencode/build-home \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build /tmp/opencode/build-home -j3
```

Notes:

- Keep at most `-j3` on small boxes; CI/packaging may raise it.
- Keep ccache enabled (`CMAKE_CXX_COMPILER_LAUNCHER=ccache`).
- Do not use the in-repo `build/` directory (stale, git-ignored).
- The binary is `progressive-cli`; `matrixcli` is a compat symlink.

## Usage

```bash
# Start with TUI
./progressive-cli tui

# Start API server
./progressive-cli serve --port 8080

# Remote ASCII UI (server holds the session in RAM, thin client renders)
./progressive-cli serve --ttys --port 29325 [--token X]
./progressive-cli ttys --host 127.0.0.1 --port 29325 [--token X]

# Login (username accepts localpart "me" or full @me:matrix.org; --mxid alias)
./progressive-cli login --homeserver https://matrix.org --username me --password xxxxx
./progressive-cli login --homeserver https://matrix.org --mxid @me:matrix.org --password xxxxx
# ...or interactively (password hidden):
./progressive-cli login --homeserver https://matrix.org --interactive

# Native MAS login (no browser) and MAS registration (email + captcha)
./progressive-cli login --mas --homeserver https://xmr.se --username me --password xxxxx
./progressive-cli register --mas --homeserver https://xmr.se --username newuser --password xxxxx --email you@xmr.se

# Register a new account (m.login.dummy)
./progressive-cli login --register --homeserver https://matrix.org --username newuser --password xxxxx

# Register with a registration token (m.login.registration_token)
./progressive-cli login --register --homeserver https://matrix.org --username newuser --password xxxxx --reg-token s3cret

# Join / knock (aliases are URL-encoded, so #room:server works)
./progressive-cli join "#room:server" "reason"
./progressive-cli knock "#room:server"

# Route ALL traffic through Tor (SOCKS5, DNS resolved by the proxy — no leaks)
./progressive-cli proxy on --host 127.0.0.1 --port 9050 --type socks5h
./progressive-cli proxy status
./progressive-cli proxy off

# Copy the last N cached messages to the clipboard
./progressive-cli copy "#room:server" 20 [--preview]

# Shell completion (bash/zsh/fish)
./progressive-cli completion bash [--install]

# Demo mode (offline, uses its own matrixcli-demo.db — never the real cache)
./progressive-cli demo

# Check status
./progressive-cli status
```

The bridges (`irc`, `lemmy`, `td`, `dc`) are experimental. TDLib uses the
public test `api_id`/`api_hash` unless config.json `tdlib_api_id` /
`tdlib_api_hash` override them with your own credentials from
https://my.telegram.org.

## Dependencies

- CMake 3.20+
- C++23 compiler (GCC 13+ / Clang 17+)
- OpenSSL
- libolm 3.2.16 via progressive-core (for E2EE)
- nlohmann/json, simdjson (fetched automatically)
- ncurses (for TUI mode, optional)
- sqlite3, libcurl

## License

AGPL-3.0-or-later (see [LICENSE](../LICENSE)).

This is a derivative work of gomuks (AGPLv3) and of the Progressive Chat
Android client's native C++ layer (AGPLv3).
