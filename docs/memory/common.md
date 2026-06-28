# Common Project Memory

Most work is not platform-specific. Linux and Windows agents can both edit code, write docs, inspect the repo, build plans, and reason from committed artifacts. Platform only matters when a task needs local tools or a running game/debugger session.

Source-of-truth rules:

- Prefer checked-out code and committed docs over archived memory.
- Treat `docs/memory/archive/*` as historical context, not instructions.
- The imported memory was reconciled on 2026-06-28 against `master@931438d`.
- ERR no-bake Phase 2 is already on `master`; `src/generated/goblin_map_data.cpp` is a stub and `docs/nobake_scoreboard.md` is the current coverage reference.
- If an archive note says `HEAD 4a7716d`, keep the no-bake conclusion but ignore the commit pointer.
- If `archive/windows/next-session-resume.md` says "4 branches above master", treat it as stale for this checkout.

Collaboration rules:

- Work on feature branches, not directly on `master`, unless explicitly told otherwise.
- Do not push unless explicitly asked.
- Commit only after a useful checkpoint or when asked.
- Runtime/gameplay visual validation is usually user-owned; agents can prepare builds, logs, probes, and clear test instructions.
- When the user names a direct fix, implement it with minimal debate.

Memory update rule:

- After a completed task, update memory if the task changed durable project state.
- Update `common.md` for shared state, workflow rules, blockers, branch/merge status, or cross-platform facts.
- Update `linux.md` only for Linux/Proton-specific capabilities, limitations, build/deploy notes, or gotchas.
- Update `windows.md` only for Windows-specific Ghidra/RPM/runtime RE tooling, paths, capabilities, or gotchas.
- Do not copy every detail into active memory. Keep active memory short; put long investigations in normal docs
  (`docs/re/*`, feature docs, or committed code comments) and link them from memory when useful.
- If a task only edits code without changing durable context, a memory update is not required.

Useful archive entry points:

- `archive/linux/MEMORY.md`: Linux-side historical index.
- `archive/windows/MEMORY.md`: Windows-side historical index.
- `archive/windows/workflow-preferences.md`: collaboration patterns.
- `archive/linux/overlay-rendered-markers.md`: overlay/no-bake architecture history.
- `archive/windows/nobake-endgame-roadmap.md`: no-bake phase history.
