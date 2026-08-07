#!/bin/bash
# tests/test_cli_e2e.sh — CLI end-to-end test against a live homeserver.
#
# Drives the REAL matrixcli binary through the ecore-backed command surface:
#   login (E2EE bootstrap) -> status --json -> e2ee status --json ->
#   backup create (recovery key) -> sync (one-shot cache fill) ->
#   search-public (directory)
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

# Graceful skip without a homeserver (same pattern as test_synapse_e2ee).
if ! curl -sf "$HS/_matrix/client/versions" >/dev/null 2>&1; then
    echo "SKIP: no homeserver reachable at $HS (set SYNAPSE_URL)"
    exit 0
fi
echo "server up: $HS"

fail=0
check() {
    local label="$1"; shift
    local out rc=0
    out=$(eval "$*" 2>&1) || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "ok: $label"
    else
        echo "FAIL: $label"
        echo "--- output:"
        echo "$out" | tail -6
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

check "login (E2EE bootstrap)" \
    "$BIN login --homeserver $HS --username $USER --password $PASS 2>&1 | grep -q 'Logged in as'"
check "status --json logged_in" \
    "$BIN status --json 2>/dev/null | grep -q '\"logged_in\":true'"
check "e2ee status ready" \
    "$BIN e2ee status --json 2>/dev/null | grep -q '\"ready\":true'"
check "backup create recovery key" \
    "$BIN backup create 2>/dev/null | grep -qE '[A-Za-z0-9+/=]{40,}'"
check "sync one-shot" \
    "$BIN sync 2>/dev/null | grep -q 'Synced'"
check "rooms --json after sync" \
    "$BIN rooms --json 2>/dev/null | grep -q '\"total\":0'"
check "search-public runs" \
    "$BIN search-public test --json 2>/dev/null | grep -q '{'"

cd / || exit 1
rm -rf "$T"

if [ "$fail" -eq 0 ]; then
    echo "=== ALL CLI E2E TESTS PASSED ==="
    exit 0
fi
echo "=== CLI E2E TEST(S) FAILED ==="
exit 1
