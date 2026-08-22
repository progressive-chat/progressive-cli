# AGENTS.md

## Machine constraints

This box has exactly **4 CPU cores**. Never run more than `-j3` for ninja/make, and do not launch parallel work beyond 3 jobs total. Oversubscribing freezes the box and kills long builds.

- Keep ccache enabled: configure with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.
- Use the throwaway build dir `/tmp/opencode/build-home` (the in-repo `cppcli/build/` is stale/broken).
- Interact with a running build via short polling sleeps (e.g. 240s), never background multiple heavy compiles.

## Repo layout

- `cppcli/` — C++ CLI (src + tests mirror)
- `docs/` — README features, `status.json` is generated from `features.json`

## Language and publishing rules

- **English only in the repo.** Everything that gets published to the internet —
  commit messages, code, comments, docs and PR descriptions — must be written in
  English. Do not leave Russian/Cyrillic (or other non-English) literals in source
  or commit text. Cyrillic is only allowed as numeric Unicode code points (e.g.
  `0x0439`) when a mapping table genuinely needs a specific code point.
- **Commit and push to the remote.** Finish each task with a commit and push it to
  the remote (`origin/main`). Do not leave work unpushed.
- **Keep files small.** Try to keep source files under ~1000 lines. When a file
  exceeds 1000 lines, split it into focused modules instead of growing it further.

## Dependencies

- `progressive-core` is fetched as a prebuilt artifact (`progressive-core-artifact`);
  edits to `build/_deps/progressive-core-src` do NOT relink into the CLI. Put
  runtime logic in `cppcli/` (our own `lib/` + `src/`), not in the vendored core,
  unless the core is being rebuilt from source on purpose.