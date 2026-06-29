# Agent Handoff

This repo uses `docs/memory/` for project memory.

Read first:

- `docs/memory/common.md`
- `docs/memory/linux.md` when running on Linux/Proton.
- `docs/memory/windows.md` when running on Windows.

Drill into the topic folders for detail: `docs/memory/{features,bugs,tooling,process}/` (each has a `README.md` index). See `docs/changelog.md` for the fork's feature/change/fix list. Notes are sanitized and may contain stale pointers; checked-out code and committed docs win on conflict.

Single memory store (important):

- Project memory lives ONLY in `docs/memory/` (committed, shared across machines) + `docs/changelog.md`.
- Do NOT create or write to a separate per-agent / per-machine memory store — no local agent memory,
  Serena memories, `~/.claude` memory, or imported tar/rar dumps. The old separate Linux + Windows
  memories were merged into this repo on 2026-06-29 and must not diverge again.
- Write durable notes to the matching `docs/memory/{features,bugs,tooling,process}/` file and commit.

Platform rule:

- Most tasks are possible on both Linux and Windows.
- Windows is preferred for Ghidra, Cheat Engine, Python RPM against a running game, and runtime RE.
- Linux is fine for normal code/docs work, cross-builds, log analysis, and preparing RE prompts for Windows.

Workflow:

- Work on a feature branch unless told otherwise.
- Do not push unless explicitly asked.
- Keep changes scoped.
- At the end of a completed task, update `docs/memory/` when the result changes project state, workflow,
  blockers, machine capabilities, or important next steps. If it adds a feature or fixes a bug, also add
  a line under `[Unreleased]` in `docs/changelog.md`.
- For RE handoffs, write clear prompts/findings under `docs/re/`.
