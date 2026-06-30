# Agent Handoff

This repo uses `docs/memory/` for project memory.

Read first:

- `docs/HANDOFF.md` — live cross-session work queue (in-progress + what's next + why). Read this FIRST to
  resume; update it at the end of a session.
- `docs/memory/common.md`
- `docs/memory/linux.md` when running on Linux/Proton.
- `docs/memory/windows.md` when running on Windows.

Drill into the topic folders for detail: `docs/memory/{features,bugs,tooling,process}/` (each has a `README.md` index). See `docs/changelog.md` for the fork's feature/change/fix list. Notes are sanitized and may contain stale pointers; checked-out code and committed docs win on conflict.

## Design principles (prime directive)

- **Mod-agnostic first.** MapForGoblins must work on ANY Elden Ring mod (and vanilla), not just ELDEN
  RING Reforged. ERR is the dev install, NOT the target boundary. Anything ERR-specific (hardcoded names
  like `MENU_MAP_ERR_*`, ERR-tuned constants, ERR-only assets) is acceptable only as an additive layer on
  top of a mod-agnostic base — never as the only path.
- **Runtime/Disk over baked.** Read icons, glyphs, markers, and param data from the ACTIVE install's real
  files — resident GPU textures, or disk via the Oodle/dvdbnd no-bake path — so they are automatically
  correct for whatever mod is loaded. A baked snapshot (the overlay icon atlas, static map-data bakes) is
  an ERR-frozen artifact: stale or wrong under any other mod. Baked is a transitional fallback to be
  eliminated, not a source of truth.
- **Circle is the universal fallback.** When no glyph resolves from the active files, draw the plain
  circle (needs no art, correct for every mod). Prefer `native-from-disk → circle` over
  `native → baked → circle`; the baked middle tier only masks mod-agnostic gaps.
- **Acceptance test for any icon/data path:** "does this still produce a correct result on a DIFFERENT mod
  with different params/textures?" If it only works because the values happen to match ERR, it is not done.

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
- **Plans live on `master`, not on plan-only branches.** A branch that only holds a planning/design doc
  drifts as master's memory/inventory evolves (a plan goes stale against `docs/memory/`). So land the
  plan on `master` under `docs/`, note it in `docs/HANDOFF.md`, and fork a fresh implementation branch
  from master only when the work actually starts. Don't keep a long-lived plan-only branch.
- **The USER pushes.** Committing is fine; pushing to a remote is the user's job. Do not push unless
  explicitly asked, and do NOT end messages reminding the user to push — assume they will. State the
  local state (e.g. "committed, local master is ahead of origin") once and move on; no nagging.
- **Merged-branch hygiene.** When a branch's work has landed on `master`, the branch is disposable —
  delete it (local + remote) so refs don't pile up. Detect "already merged" two ways, because identity
  alone misses squashed/rebased branches:
  - `git branch --merged master` — catches fast-forward / true-merge branches (commit identity).
  - `git cherry master <branch>` — every line prefixed `-` means that patch is ALREADY in master by
    patch-id (squash/rebase lands content under new SHAs, so `--merged` won't see it). All `-` ⇒ safe.
  Keep only `master`, long-lived branches, and branches with genuinely-new commits (`git cherry` `+`).
  Local delete `git branch -d` (use `-D` only after confirming via cherry); remote delete
  `git push origin --delete <b>`. Never delete a branch with `+` (unmerged) work or the default branch.
- Keep changes scoped.
- At the end of a completed task, update `docs/memory/` when the result changes project state, workflow,
  blockers, machine capabilities, or important next steps. If it adds a feature or fixes a bug, also add
  a line under `[Unreleased]` in `docs/changelog.md`.
- For RE handoffs, write clear prompts/findings under `docs/re/`.
