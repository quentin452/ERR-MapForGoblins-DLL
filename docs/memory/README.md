# Memory

This directory keeps project memory for future agents. Read the short active files first; use the archive only when you need detail.

Active files:

- [common.md](common.md): shared project state and rules that apply on every machine.
- [linux.md](linux.md): Linux/Proton agent notes.
- [windows.md](windows.md): Windows agent notes, especially Ghidra/RPM/runtime RE.

Historical archive:

- `archive/linux/`: sanitized import from `<downloads>/memoryagentlinux.tar.gz` (Read-only).
- `archive/windows/`: sanitized import from `<downloads>/memoryagentwindows.rar` (Read-only).

Privacy scrub before first commit:

- Removed session IDs from memory metadata.
- Replaced personal usernames, SteamID64 values, and user-home paths with placeholders.
- Replaced machine-specific local roots with placeholders such as `<repo>`, `<ERR_ROOT>`,
  `<downloads>`, `<ghidra_project>`, and `<ghidra_scripts>`.

Local reconciliation at import:

- Current checkout: `master` at `931438d` (`Remove enemy_names_i18n.json and update build pipeline dependencies`).
- No-bake Phase 2 for ERR is already on `master`; `src/generated/goblin_map_data.cpp` is a stub and `docs/nobake_scoreboard.md` is the current coverage source.
- Windows `next-session-resume.md` was stale for this checkout: it described four branches above master and an unmerged local `feat/dvdbnd-packed-reader`. In this repo, `feat/quests`, `feature/spatial-grid-opti`, and `feature/dx-bugs-backlog` are not ahead of master; `origin/feat/dvdbnd-packed-reader` exists remotely but is not merged into master.
- Linux memories that mention `HEAD 4a7716d` predate the current master tip; treat the no-bake facts as still relevant, but not the commit pointer.

Use checked-out code and committed docs as source of truth when archive notes conflict with reality.
