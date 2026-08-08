# matrixcli

A C++20 Matrix CLI client with TUI and HTTP API server.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

```bash
# Start with TUI
./matrixcli

# Start API server
./matrixcli serve --port 8080

# Login
./matrixcli login --homeserver https://matrix.org --username @user:matrix.org --password xxxxx

# Register a new account (m.login.dummy)
./matrixcli login --register --homeserver https://matrix.org --username newuser --password xxxxx

# Register with a registration token (m.login.registration_token)
./matrixcli login --register --homeserver https://matrix.org --username newuser --password xxxxx --reg-token s3cret

# Route ALL traffic through Tor (SOCKS5, DNS resolved by the proxy — no leaks)
./matrixcli proxy on --host 127.0.0.1 --port 9050 --type socks5h
./matrixcli proxy status
./matrixcli proxy off

# Check status
./matrixcli status
```

## Dependencies

- CMake 3.20+
- C++20 compiler (GCC 11+ / Clang 14+)
- OpenSSL
- libolm (for E2EE)
- nlohmann/json (fetched automatically)
- ncurses (for TUI mode, optional)

## License

AGPL-3.0-or-later (see [LICENSE](../LICENSE)).

This is a derivative work of gomuks (AGPLv3) and of the Progressive Chat
Android client's native C++ layer (AGPLv3).
