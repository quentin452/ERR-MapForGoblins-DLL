# Agent Handoff

This repo uses `docs/memory/` for project memory.

Read first:

- `docs/HANDOFF.md` — live cross-session work queue (in-progress + what's next + why). Read this FIRST to
  resume; update it at the end of a session.
- `docs/memory/common.md`
- `docs/memory/linux.md` when running on Linux/Proton.
- `docs/memory/windows.md` when running on Windows.

Drill into the topic folders for detail: `docs/memory/{features,bugs,tooling,process}/` (each has a `README.md` index). See `docs/changelog.md` for the fork's feature/change/fix list. Notes are sanitized and may contain stale pointers; checked-out code and committed docs win on conflict.

## Driving the live game — debug RPC (read before any runtime work)

The DLL exposes a **dev-only TCP loopback RPC** that lets you drive and inspect the RUNNING game: read/
write params live, `give_item`/`goods_count`, `warp`, `loot_at`, `refresh_markers`, `fmg_set`, `sidecar`,
inject keyboard/mouse, `screenshot`, and arm find-what-accesses breakpoints. This is the primary way
runtime RE and in-game verification happen here (proven on Linux/Proton — the game runs under Proton on
the dev box). A background job can even cold-boot ER and run a self-contained RPC session.

- **Command catalog + how to enable/drive it:** `docs/memory/tooling/rpc-commands.md`.
- **⚠ FRESHNESS FIRST:** before RPC-verifying code you just changed, run `mfg.py rpc mfg_build` — `ping`
  answers from a STALE DLL too (the listener lives, but the running DLL may predate your edits → your new
  verbs are `unknown`). A rebuild needs a game **restart**/hot-reload to load; a redeploy alone keeps the
  old DLL. Also confirm you're **in-world**, not the main menu (`status`). See the hardening note below.
- **Driver:** `tools/mfg.py` (`rpc` one-shot / `repl` / `run <script>` / `test`). Enable with ini
  `[Debug] debug_rpc_port = 38700`.
- **Scripting gotchas (mandatory):** `docs/memory/tooling/mfg-rpc-driver-hardening.md` — the game can
  freeze with the listener still answering `ping`, so gate on real liveness; input has AZERTY/refocus
  quirks; a bg job keeps ER alive only via a single foreground blocking command.

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
- **Token economy (Session Recovery):** When converting raw conversation transcripts (.jsonl) into Markdown recovered session logs, filter and truncate large `base64` strings (replace with `[Base64 Image Truncated]`) and deduplicate repeated system attachments (`deferred_tools_delta`, `skill_listing`) to avoid multi-MB logs. Session recovery notice blocks in `CLAUDE.md` must stay under 8 lines by nesting detailed subagent listings and technical summaries inside the main Markdown recovery file.

Platform rule:

- Most tasks are possible on both Linux and Windows.
- Runtime RE is PROVEN on Linux via in-DLL probes (the game runs here under Proton): param/EMEVD/
  FMG scans, dumps, asset radar — `src/goblin_param_scan.cpp` + `docs/memory/tooling/
  linux-runtime-re-options.md`. Group 2 (Elevators/Smithing) was solved end-to-end this way
  (2026-07-02, `docs/re/linux_group2_prompt_binding_re_findings.md`).
- Windows keeps: Cheat Engine GUI comfort (interactive scans/structure dissection; ceserver from
  Linux untried), the Ghidra project/scripts that already live there, and Oodle-compressed asset
  EXTRACTION (in-process decompression works on Linux; offline does not).
- Linux is fine for normal code/docs work, cross-builds, log analysis, and preparing RE prompts for Windows.

Workflow:

- Work on a feature branch unless told otherwise.
- **Plans live on `master`, not on plan-only branches.** A branch that only holds a planning/design doc
  drifts as master's memory/inventory evolves (a plan goes stale against `docs/memory/`). So land the
  plan on `master` under **`docs/plans/`** (see its `README.md` index), note it in `docs/HANDOFF.md`, and
  fork a fresh implementation branch from master only when the work actually starts. Don't keep a
  long-lived plan-only branch. (Research/findings/RE recipes are NOT plans — keep those in `docs/`,
  `docs/re/`, or `docs/memory/`.)
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
- **Docs follow implementation (same commit/PR, not a later pass).** Any implementation that
  touches a RE'd domain MUST update the matching docs IN THE SAME change, or they rot (the
  `docs/re/README.md` coverage map drifted ~109 commits stale because shipping work never
  reflected it). The mapping, by what the code change does:
  - **Resolves/advances an RE item** (an OPEN/Remaining gap becomes solved, shipped, or a
    documented dead end) → update `docs/re/README.md` (flip the status, fix the counts —
    doc count / sig count, correct the Mapped table) AND the relevant `docs/re/*_findings.md`
    (a dead end needs its own findings note; a shipped mechanism may correct the planned one,
    e.g. timestep freeze ≠ dt-zero).
  - **Adds/changes a subsystem** (new parser, new input path, new source of truth) → update
    the `✅ Mapped` table / frontier list in `docs/re/README.md` (a subsystem is either
    mapped-and-shipped or listed as missing; there is no silent middle ground).
  - **Changes the frontier framing** (a wall falls, a route dead-ends) → update the "Rule of
    thumb" line + the corresponding frontier item in `docs/re/README.md`.
  - **Every case:** update `docs/HANDOFF.md` (close/advance the queue item), add the
    user-facing line to `docs/changelog.md` under `[Unreleased]`, and write the durable note
    to `docs/memory/` per the single-store rule. When in doubt, mirror what the change ships.
  - Verification: after the doc edits, re-check any filename/sig/count you touched actually
    exists/matches (a stale pointer in a fresh edit is still stale).
- At the end of a completed task, update `docs/memory/` when the result changes project state, workflow,
  blockers, machine capabilities, or important next steps. If it adds a feature or fixes a bug, also add
  a line under `[Unreleased]` in `docs/changelog.md`.
- For RE handoffs, write clear prompts/findings under `docs/re/`. See `docs/re/README.md` — the RE
  COVERAGE MAP (what of ELDEN RING is reverse-engineered vs the frontier: MSB-write + ESD are the walls).
