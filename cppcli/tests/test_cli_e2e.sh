#!/bin/bash
# tests/test_cli_e2e.sh — CLI end-to-end test against a live homeserver.
#
# Drives the REAL matrixcli binary through the ecore-backed command surface:
#   register (--register) -> login (E2EE bootstrap) -> status --json ->
#   e2ee status --json -> backup create (recovery key) -> sync (one-shot
#   cache fill) -> search-public (directory)
#
# Env: SYNAPSE_URL (default http://localhost:8008). Skips gracefully when no
# homeserver is reachable. Run from a fresh temp dir so session.db /
# config.json / matrixcli.db never touch the repo.
#
# Usage: test_cli_e2e.sh <path-to-matrixcli-binary>
set -u

HS="${SYNAPSE_URL:-http://localhost:8008}"
BIN="${1:-}"
PASS="cli_e2e_pass_42"
USER="cli_e2e_$(date +%s)"

if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "FAIL: matrixcli binary not found: $BIN"
    exit 1
fi
# The test runs from a temp dir — the binary path must be absolute.
case "$BIN" in
    /*) ;;
    *) BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" ;;
esac

# Graceful skip without a homeserver (same pattern as test_synapse_e2ee).
if ! curl -sf "$HS/_matrix/client/versions" >/dev/null 2>&1; then
    echo "SKIP: no homeserver reachable at $HS (set SYNAPSE_URL)"
    exit 0
fi
echo "server up: $HS"

fail=0
# check <label> <command> <grep-pattern>: runs command, greps its output
# (stdout+stderr), prints the output on mismatch.
check() {
    local label="$1" cmd="$2" pattern="$3" out rc=0
    out=$(eval "$cmd" 2>&1) || rc=$?
    if echo "$out" | grep -qE "$pattern"; then
        echo "ok: $label"
    else
        echo "FAIL: $label (rc=$rc)"
        echo "--- output:"
        echo "$out" | tail -8
        fail=1
    fi
}

# Register a fresh user (dummy auth, enabled in the test synapse config).
REG=$(curl -s -X POST "$HS/_matrix/client/v3/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$USER\",\"password\":\"$PASS\",\"auth\":{\"type\":\"m.login.dummy\"}}")
if ! echo "$REG" | grep -q '"user_id"'; then
    echo "FAIL: registration ($REG)"
    exit 1
fi
echo "registered: $USER"

T=$(mktemp -d)
cd "$T" || exit 1

# Register a SECOND user through the CLI itself (--register, m.login.dummy).
REGUSER="cli_reg_$(date +%s)"
check "register via CLI (m.login.dummy)" \
    "$BIN login --register --homeserver $HS --username $REGUSER --password $PASS" \
    "Registered as"
check "register via CLI --json" \
    "$BIN login --register --homeserver $HS --username ${REGUSER}2 --password $PASS --json" \
    '"user_id"'

check "login (E2EE bootstrap)" \
    "$BIN login --homeserver $HS --username $USER --password $PASS" \
    "Logged in as"
check "status --json logged_in" \
    "$BIN status --json" \
    '"logged_in":true'
check "e2ee status ready" \
    "$BIN e2ee status --json" \
    '"ready":true'
check "backup create recovery key" \
    "$BIN backup create" \
    '[A-Za-z0-9+/=]{40,}'
check "sync one-shot" \
    "$BIN sync" \
    "Synced"
check "rooms --json after sync" \
    "$BIN rooms --json" \
    '"total":0'
check "search-public runs" \
    "$BIN search-public test --json" \
    '{'

cd / || exit 1
rm -rf "$T"

if [ "$fail" -eq 0 ]; then
    echo "=== ALL CLI E2E TESTS PASSED ==="
    exit 0
fi
echo "=== CLI E2E TEST(S) FAILED ==="
exit 1
