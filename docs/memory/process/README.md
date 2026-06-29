# Process

Campaign history, architecture direction, planning verdicts, and working lessons. These are not
feature/bug/tool docs — they record *how* the project decided and worked. Treat plan verdicts as the
intended sequencing; treat campaign notes as history (the durable outcome is in `changelog.md` and the
feature docs).

## No-bake campaign (history)
- **Provenance scoreboard** [superseded] — per-marker `{Baked,DiskMSB,Live}` system that drove baked
  markers ~8419 → 16 → 0. → [nobake-coverage-scoreboard](nobake-coverage-scoreboard.md)
- **3-phase endgame plan** [superseded] — P1 baked→0 ✅, P2 delete static bake ✅ (6.19→3.76 MB),
  P3 paramdef-driven offsets is still aspirational. → [nobake-endgame-roadmap](nobake-endgame-roadmap.md)
- **Residual triage lens** [active] — an "irreducible" residual is provisional: classify (A) unparsed
  disk data vs (B) un-RE'd runtime path, name the blocker before accepting. → [residual-irreducible-strategy](residual-irreducible-strategy.md)
- **Externalize-mapdata plan** [superseded] — mooted for ERR by no-bake; forward path is migrating
  vanilla/ERTE/Convergence onto the same DiskMSB+live pipeline. → [one-dll-externalize-mapdata](one-dll-externalize-mapdata.md)
- **Observations explained** [resolved] — Furnace Golem / Moore / Black Syrup were expected behaviour,
  not bugs. → [map-data-obs-moore-enemies](map-data-obs-moore-enemies.md)

## Architecture direction
- **Live params vs baked data** [active] — target is HYBRID: params/FMG live, MSB geometry + EMEVD
  correlations + iconId curation stay baked. Only bosses (`textId2==5100`) and graces are live-portable. → [live-param-vs-baked-data](live-param-vs-baked-data.md)
- **Engine/project direction** [strategy] — ER mod vs EvoCraft vs a new 2.5D engine; locked: a 2.5D+ImGui
  engine is a separate repo, reuses MapForGoblins DX12/atlas/ImGui bricks. → [engine-direction-decisions](engine-direction-decisions.md)

## Planning verdicts (the active feature-branch backlog)
> Intended sequencing: **spatial-grid first** (foundation) → DX clustering reusing its cells → remaining
> DX bugs → quests. >10k markers justifies the grid.
- **Plans-to-audit** [superseded] — three plans audited & rewritten to v2. → [plans-to-audit](plans-to-audit.md)
- **Spatial-grid audit** [active] — strongest of the three; do first; O(N)→O(visible). → [plan-spatial-grid-audit](plan-spatial-grid-audit.md)
- **DX-bugs audit** [active] — split into 5 PRs; reject the global QPC pause hook. → [plan-dx-bugs-audit](plan-dx-bugs-audit.md)
- **Quests audit** [active] — reuse already-shipped infra; event-flag writes must default READ-only. → [plan-quests-audit](plan-quests-audit.md)

## Working lessons
- **Collaboration & branch hygiene** [active] — flat feature branches off master, never push, confirm
  genuine decisions, verify offsets live not statically. → [workflow-preferences](workflow-preferences.md)
- **Disambiguate symptoms first** [active] — ask 1-3 sharp questions before a build-deploy-test cycle. → [disambiguate-bug-symptoms-first](disambiguate-bug-symptoms-first.md)
