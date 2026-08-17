# AGENTS.md

## Machine constraints

This box has exactly **4 CPU cores**. Never run more than `-j3` for ninja/make, and do not launch parallel work beyond 3 jobs total. Oversubscribing freezes the box and kills long builds.

- Keep ccache enabled: configure with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.
- Use the throwaway build dir `/tmp/opencode/build-home` (the in-repo `cppcli/build/` is stale/broken).
- Interact with a running build via short polling sleeps (e.g. 240s), never background multiple heavy compiles.

## Repo layout

- `cppcli/` — C++ CLI (src + tests mirror)
- `docs/` — README features, `status.json` is generated from `features.json`