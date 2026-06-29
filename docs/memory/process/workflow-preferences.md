---
name: workflow-preferences
description: "Project collaboration preferences — decisions, feature branches, and runtime testing"
metadata: 
  node_type: memory
  type: feedback
---

How to work on this repo:

- **Confirm genuine decisions** with a question (AskUserQuestion) before acting — he engages with
  multiple-choice options and often proposes a refined third path. Don't over-ask trivial things.
- **Commits land on feature branches** (`feat/...`), usually committed **locally**; **he pushes
  himself** (or has hooks). Don't push without being asked. Source fixes for the data pipeline are
  often committed on Linux per the task brief — when a brief says "don't push / lands via Linux",
  deliver files + a diff instead.
- **In-game *visual* testing is user-owned** — remote agents can't see the screen or drive gameplay. BUT
  they can read live game memory via RPM while the game runs (see [[rpm-live-memory-tooling]]) — use it to
  verify chains/offsets instead of shipping unverified static guesses ([[player-pos-static-unreliable]]).
- Tasks arrive as **RE prompt docs** `docs/re/windows_*_prompt.md`, usually committed first. The user
  may paste the **commit hash** ("check <hash>"); run `git show`, read the brief, run the Ghidra/RPM
  analysis, write a `docs/re/*_findings.md`, and **commit only on explicit "go"/"commit"**. The user often
  replies with the next brief's hash after a CE/runtime test refutes or confirms.
- Briefs may use `Co-Authored-By: Claude Opus 4.8 (1M context)`; findings commits use
  `Co-Authored-By: Claude Opus 4.8`. The CRLF warning on commit is harmless.

**Branch hygiene (added 2026-06-25):**
- **Before creating a new branch, check `git branch --show-current`.** If already on a non-master
  branch, do NOT branch off it (that builds a chain/topology that diverges). Instead: if the current
  branch's feature is **runtime-verified**, merge it to master FIRST, then cut the new branch from
  master. Keep the topology flat (every feature branches off master).
- **Feature done = merge to master** (once runtime-verified). Don't leave finished, validated work
  stranded on a feature branch. While the merge settles, **check the new runtime logs** for the next
  signal/regression — the verify→merge→watch-logs loop is the cadence.

**Act, don't lecture (added 2026-06-27, learned the hard way):**
- When a fix is direct — and ESPECIALLY when the user names it ("use the native getter", "supprime le RPM")
  — **implement it immediately with near-zero prose.** Do not answer a directive with more RE analysis,
  decompiler dumps, trade-off essays, or end-of-turn option menus. That reads as stalling. The desired
  pattern is a one-line fix when the path is obvious (example: RPM-to-self → direct in-process read).
- Default to **terse**: a short result line, not a report. Save the long write-up for `docs/re/*` when
  a brief actually asks for findings. One recommendation, not a survey.

**Why:** the project iterates fast across many sub-tasks, and machine-specific + game-facing steps are
usually handled outside the repo.
**How to apply:** front-load the decisions, do the heavy static/tooling work, hand off the
machine/game-specific confirmation with crisp instructions.
