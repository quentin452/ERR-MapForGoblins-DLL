# Memory

Project memory for future agents. Read the short active files first, then drill into the topic folders.

## Active files

- [common.md](common.md) — what the project is, shared state, workflow rules, and the index.
- [linux.md](linux.md) — Linux/Proton machine + tooling specifics.
- [windows.md](windows.md) — Windows machine + runtime-RE tooling specifics.
- [../changelog.md](../changelog.md) — every fork-specific feature/change/fix (`[Unreleased]`).

## Topic folders

Each folder has a `README.md` that reconciles its notes into a single current-truth index; the files
under it are the relocated raw RE/agent notes.

- [features/](features/README.md) — fork functionality and the RE that became a feature.
- [bugs/](bugs/README.md) — complex bugs, resolved and open.
- [tooling/](tooling/README.md) — Ghidra, RPM, Cheat Engine, build pipelines, parsers, offsets.
- [process/](process/README.md) — no-bake campaign history, architecture direction, plan verdicts, lessons.

## History & provenance

These notes were imported from two separate machine memories (Linux + Windows agents) and reconciled
against the live code on 2026-06-29; the old `archive/{linux,windows}/` split was flattened into the
four folders above. **Checked-out code and committed docs win on any conflict** with a note.

Privacy: session IDs removed; usernames, SteamID64 values and home paths replaced with placeholders
(`<repo>`, `<ERR_ROOT>`, `<downloads>`, `<ghidra_project>`, `<ghidra_scripts>`).
