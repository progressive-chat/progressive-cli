#!/usr/bin/env bash
# scripts/reimport-ecore.sh — re-import progressive-desktop core changes into
# cppcli/lib/ecore (byte-identical copies, provenance recorded in README).
#
# The vendored core is a byte-identical copy of the desktop's Qt-free layer.
# When the desktop core moves forward, this script diffs every vendored file
# against the desktop tree, copies the changed ones, and updates the
# provenance hashes in lib/ecore/README.md.
#
# Usage:
#   ./scripts/reimport-ecore.sh            # dry run: report differences
#   ./scripts/reimport-ecore.sh --apply    # copy changed files + update README
#   ./scripts/reimport-ecore.sh --apply --rebuild   # ... then rebuild + ctest
#
# Env: PROGRESSIVE_DESKTOP_DIR (default ~/progressive-desktop)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
D="${PROGRESSIVE_DESKTOP_DIR:-$HOME/progressive-desktop}"
PN="$D/third_party/progressive-android-experiments/progressive/src/main/cpp"
E="$ROOT/cppcli/lib/ecore"

APPLY=0
REBUILD=0
for a in "$@"; do
    case "$a" in
        --apply) APPLY=1 ;;
        --rebuild) REBUILD=1 ;;
    esac
done

[ -d "$D/src/core" ] || { echo "FAIL: desktop repo not found at $D (set PROGRESSIVE_DESKTOP_DIR)"; exit 1; }
[ -d "$PN/src" ] || { echo "FAIL: native submodule not found under $D"; exit 1; }

DESKTOP_HEAD=$(git -C "$D" rev-parse HEAD)
NATIVE_HEAD=$(git -C "$D/third_party/progressive-android-experiments" rev-parse HEAD)

echo ">> Desktop core: ${DESKTOP_HEAD:0:7} (git)"
echo ">> Native:       ${NATIVE_HEAD:0:7} (git)"
echo ">> Mode:         $([ "$APPLY" -eq 1 ] && echo APPLY || echo dry-run)"

changed=0; copied=0; diffs=0
copy_if_changed() {
    local src="$1" dst="$2"
    if [ ! -f "$src" ]; then echo "  WARN: source missing: $src"; return; fi
    if ! cmp -s "$src" "$dst"; then
        diffs=$((diffs+1))
        echo "  DIFF: ${dst#$E/}  (desktop ${src#$D/})"
        if [ "$APPLY" -eq 1 ]; then
            cp "$src" "$dst"
            copied=$((copied+1))
        fi
    fi
    changed=$((changed+1))
}

echo ">> Comparing $(find "$E/core" "$E/native" -name '*.cpp' -o -name '*.hpp' | wc -l) vendored files..."

# ecore/core/* <-> desktop src/core/*
for f in $(cd "$E/core" && find . \( -name '*.cpp' -o -name '*.hpp' \) | sed 's|^\./||' | sort); do
    case "$f" in
        version.h) continue ;;   # CLI-local build marker
    esac
    copy_if_changed "$D/src/core/$f" "$E/core/$f"
done

# ecore/native/*.cpp <-> native src/*.cpp
for f in $(cd "$E/native" && ls *.cpp 2>/dev/null | sort); do
    copy_if_changed "$PN/src/$f" "$E/native/$f"
done

# ecore/native/progressive/*.hpp <-> native include/progressive/*.hpp
for f in $(cd "$E/native/progressive" 2>/dev/null && ls *.hpp | sort); do
    copy_if_changed "$PN/include/progressive/$f" "$E/native/progressive/$f"
done

# android shim (android/log.h + progressive_compat.h)
copy_if_changed "$D/third_party/android_shim/progressive_compat.h" "$E/native/progressive_compat.h"
copy_if_changed "$D/third_party/android_shim/android/log.h" "$E/native/android/log.h"

# New desktop core files not yet vendored (worth reviewing)
echo ">> New desktop core files not vendored:"
newfiles=0
for f in $(cd "$D/src/core" && find . \( -name '*.cpp' -o -name '*.hpp' \) | sed 's|^\./||' | sort); do
    [ -f "$E/core/$f" ] || { echo "  NEW: $f"; newfiles=$((newfiles+1)); }
done
[ "$newfiles" -eq 0 ] && echo "  (none)"

if [ "$APPLY" -eq 1 ]; then
    sed -i "s/desktop commit [0-9a-f]*/desktop commit ${DESKTOP_HEAD:0:7}/" "$E/README.md"
    sed -i "s/native commit [0-9a-f]*/native commit ${NATIVE_HEAD:0:7}/" "$E/README.md"
    echo ">> Provenance updated: desktop ${DESKTOP_HEAD:0:7} / native ${NATIVE_HEAD:0:7}"
fi

echo
echo ">> Compared $changed files; $diffs diff(s); $copied updated; $newfiles new core files"
if [ "$diffs" -eq 0 ] && [ "$newfiles" -eq 0 ]; then
    echo ">> ecore is in sync with the desktop"
elif [ "$APPLY" -eq 1 ] && [ "$copied" -eq "$diffs" ] && [ "$newfiles" -eq 0 ]; then
    echo ">> ecore updated to match the desktop"
else
    echo ">> ecore differs from the desktop ($([ "$APPLY" -eq 1 ] && echo "see remaining DIFF lines" || echo "run with --apply to sync"))"
fi

if [ "$REBUILD" -eq 1 ]; then
    echo ">> Rebuilding + testing..."
    cmake --build "$ROOT/cppcli/build" -j2 && (cd "$ROOT/cppcli/build" && ctest)
fi
