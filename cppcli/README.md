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

MIT
