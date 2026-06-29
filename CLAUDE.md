# MapForGoblins — Claude Code entry point

Claude Code auto-loads this file. The full agent handoff lives in `AGENTS.md`, imported below.

@AGENTS.md

## Project memory — single store

- All durable project memory lives in **`docs/memory/`** in this repo and **`docs/changelog.md`**.
- This is the ONLY memory store. Do **not** create or write to a separate per-agent / per-machine
  memory (local agent memory, Serena memories, `~/.claude` memory, imported tar/rar dumps). The old
  Linux + Windows machine memories were merged into this repo on 2026-06-29 and must not diverge again.
- Read `docs/memory/common.md` first, then the relevant `docs/memory/{features,bugs,tooling,process}/README.md`.
- On any completed task that changes durable state, update the right `docs/memory/` file (+ `changelog.md`
  if it adds a feature or fixes a bug) and commit — never stash it in a side memory.
