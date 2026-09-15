#!/usr/bin/env python3
"""Regenerate docs/status.json from docs/features.json (stdlib only).

Python fallback for docs/gen.mjs for environments without node.
Stamps version/commit/builtAt from the current tree and resolves every
feature anchor (file + pattern, relative to cppcli/) to its line number.
"""
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
CPPCLI = REPO / "cppcli"


def sh(*args):
    try:
        return subprocess.check_output(
            list(args), cwd=REPO, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception:
        return "unknown"


def resolve_anchor(anchor):
    if not anchor or not anchor.get("file") or not anchor.get("pattern"):
        return None
    path = CPPCLI / anchor["file"]
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return {**anchor, "missing": True, "line": -1}
    try:
        m = re.search(anchor["pattern"], text, re.M)
    except re.error as e:
        print(f"WARNING: bad pattern {anchor['pattern']}: {e}", file=sys.stderr)
        return {**anchor, "missing": True, "line": -1}
    if not m:
        return {**anchor, "missing": True, "line": -1}
    line = text.count("\n", 0, m.start()) + 1
    return {**anchor, "missing": False, "line": line}


def main():
    features_json = json.loads((DOCS / "features.json").read_text(encoding="utf-8"))
    version = sh("git", "describe", "--tags", "--always", "--long", "--dirty")
    commit = sh("git", "rev-parse", "HEAD")
    built_at = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )
    features = []
    for f in features_json["features"]:
        resolved = resolve_anchor(f.get("anchor")) if f.get("anchor") else None
        entry = dict(f)
        if resolved is not None:
            entry["anchor"] = resolved
        elif "anchor" in entry:
            del entry["anchor"]
        features.append(entry)
    status = {
        "generated": {
            "version": version,
            "commit": commit,
            "builtAt": built_at,
            "generator": "scripts/regen_status.py",
        },
        "note": "This file is generated. Edit docs/features.json instead.",
        "categories": features_json["categories"],
        "features": features,
    }
    (DOCS / "status.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
    print(f"docs/status.json written ({version}, {commit[:7]})")
    missing = [f for f in features if f.get("anchor", {}).get("missing")]
    if missing:
        print(f"WARNING: {len(missing)} anchor(s) unresolved:", file=sys.stderr)
        for f in missing:
            a = f["anchor"]
            print(f"  - {f.get('id', f.get('name'))} ({a['file']}: {a['pattern']})",
                  file=sys.stderr)


if __name__ == "__main__":
    main()
