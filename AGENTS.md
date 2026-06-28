# Agent Handoff

This repo uses `docs/memory/` for project memory.

Read first:

- `docs/memory/common.md`
- `docs/memory/linux.md` when running on Linux/Proton.
- `docs/memory/windows.md` when running on Windows.

Use `docs/memory/archive/` only for historical detail. The archive is sanitized and may contain stale session notes; checked-out code and committed docs win on conflict.

Platform rule:

- Most tasks are possible on both Linux and Windows.
- Windows is preferred for Ghidra, Cheat Engine, Python RPM against a running game, and runtime RE.
- Linux is fine for normal code/docs work, cross-builds, log analysis, and preparing RE prompts for Windows.

Workflow:

- Work on a feature branch unless told otherwise.
- Do not push unless explicitly asked.
- Keep changes scoped.
- For RE handoffs, write clear prompts/findings under `docs/re/`.
