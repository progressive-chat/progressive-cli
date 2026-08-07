#!/bin/sh
# Fails if any vendored ecore file (lib/ecore) includes a Qt header.
# Enforces the Qt-free core boundary for portability (ported from
# progressive-desktop/scripts/check_no_qt_in_core.sh).
set -e
matches=$(grep -rl '#include <Q\|#include "Q' cppcli/lib/ecore/ 2>/dev/null || true)
if [ -n "$matches" ]; then
    echo "FAIL: Qt header found in lib/ecore/ — the vendored core must be Qt-free:"
    echo "$matches"
    exit 1
fi
echo "OK: lib/ecore/ is Qt-free"
