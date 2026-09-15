#!/usr/bin/env python3
"""Verify docs/status.json against the current tree (stdlib only).

Python fallback for docs/check.mjs local mode (no --gen/--remote flags).
Exit codes: 0 = faithful, 1 = broken anchor, 2 = drift (regen needed).
"""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
CPPCLI = REPO / "cppcli"


def head_commit():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPO, text=True
        ).strip()
    except Exception:
        return None


def main():
    status = json.loads((DOCS / "status.json").read_text(encoding="utf-8"))
    gen = status.get("generated", {})
    print(f"page:  version={gen.get('version')} "
          f"commit={gen.get('commit', 'n/a')[:7]}")
    broken = drift = checked = 0
    for f in status.get("features", []):
        a = f.get("anchor")
        if not a or not a.get("file") or not a.get("pattern"):
            continue
        checked += 1
        path = CPPCLI / a["file"]
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            print(f"BROKEN {f.get('id')}: {a['file']} unreadable")
            broken += 1
            continue
        m = re.search(a["pattern"], text, re.M)
        if not m:
            print(f"BROKEN {f.get('id')}: \"{a['pattern']}\" missing in {a['file']}")
            broken += 1
            continue
        line = text.count("\n", 0, m.start()) + 1
        if a.get("missing") or a.get("line") != line:
            print(f"DRIFT {f.get('id')}: recorded line {a.get('line')}, now {line}")
            drift += 1
    head = head_commit()
    _ = head  # informational only; local mode checks anchors, not the stamp
    print(f"checked={checked} broken={broken} drift={drift}")
    if broken:
        return 1
    if drift:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
