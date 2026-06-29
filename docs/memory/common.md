# Common Project Memory

## What this project is

**MapForGoblins** is an in-process DLL for **ELDEN RING Reforged (ERR)** that draws a goblin-style
loot/feature map as an ImGui/DX12 overlay on the game's world map. This repo is a **fork** (`master`,
~990 commits ahead of `upstream/main`) whose defining work is the **no-bake pipeline**: markers are
derived live at runtime from the active mod's MSB / EMEVD / ItemLotParam data instead of a committed
static bake. Vanilla / ERTE / Convergence profiles still ship a baked map.

For the full list of fork-specific features, changes and fixes, see **[../changelog.md](../changelog.md)**.

## Memory layout (read this order)

- **[changelog.md](../changelog.md)** — every fork feature/change/fix; the `[Unreleased]` section is the
  human-facing summary of what this fork adds over upstream.
- **common.md** (this file) — shared state, workflow rules, and the index below.
- **[linux.md](linux.md)** / **[windows.md](windows.md)** — machine + tooling specifics only.
- Topic folders (each has a `README.md` reconciling its notes into current truth):
  - **[features/](features/README.md)** — fork functionality and the RE that became a feature.
  - **[bugs/](bugs/README.md)** — complex bugs, resolved and open. Open items are the real backlog.
  - **[tooling/](tooling/README.md)** — Ghidra, RPM, Cheat Engine, build pipelines, parsers, offsets.
  - **[process/](process/README.md)** — no-bake campaign history, architecture direction, plan verdicts, lessons.

Each folder's `README.md` is the current-truth index; the individual files under it are the relocated
raw RE/agent notes (originally split across two machine memories). On any conflict, **checked-out code
and committed docs win** over a note's original wording.

## Source-of-truth rules

- Prefer checked-out code and committed docs over any memory note.
- `src/generated/goblin_map_data.cpp` is a **stub** (no-bake Phase 2 is on `master`); baked marker
  count is 0 for ERR. `docs/nobake_scoreboard.md` is the live coverage reference.
- Topic notes may contain stale commit hashes — keep the technical conclusion, ignore the pointer.
- Known current caveats (verified 2026-06-29): stale `goblin_item_icons.*` copies still exist in
  `src/generated_erte`, `generated_vanilla`, `generated_convergence`; the Proton graying bit7-vs-bit1
  fix is still unapplied; the dvdbnd reader and the 00_Solo icon atlas live on unmerged branches only.

## Collaboration rules

- Work on feature branches, not directly on `master`, unless explicitly told otherwise.
- Do not push unless explicitly asked. Commit only after a useful checkpoint or when asked.
- Runtime/gameplay visual validation is usually user-owned; agents prepare builds, logs, probes, and
  clear test instructions.
- When the user names a direct fix, implement it with minimal debate.
- Disambiguate vague visual/behavioural reports with 1-3 sharp questions before a build-deploy-test cycle.

## Memory & changelog update rule

After a completed task that changes durable project state:

1. **Changelog** — if the task added a feature or fixed a bug, add a line under `[Unreleased]` in
   `docs/changelog.md` (see its workflow header).
2. **Topic note** — add or update the relevant file under `features/`, `bugs/`, `tooling/`, or
   `process/`, and update that folder's `README.md` index if the current-truth summary changed.
3. **Active files** — update `common.md` for shared state / workflow / branch status; `linux.md` only
   for Linux/Proton specifics; `windows.md` only for Windows RE-tooling specifics.

Keep active files and folder READMEs short. Put long investigations in the topic notes or in
`docs/re/*` and link them. If a task only edits code without changing durable context, no memory
update is needed. There is no longer a read-only `archive/` — all notes live under the four folders
and are editable; supersede stale content in place rather than leaving it to mislead.

**Single memory store.** `docs/memory/` (+ `docs/changelog.md`) is the ONLY project memory. Do not
create or write to a separate per-agent / per-machine memory (local agent memory, Serena memories,
`~/.claude` memory, imported tar/rar dumps). The previously separate Linux + Windows machine memories
were merged here on 2026-06-29 and must not diverge again — always read and write this repo's memory.
