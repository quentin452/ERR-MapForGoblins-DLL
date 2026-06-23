# Session brief — runtime item-icon coverage (force-load / binding / residency)

Wrap-up of the 2026-06-22 session that started from commit `7d46471` (resident-icon enumeration brief).
Full RE detail lives in [`windows_resident_icon_enumeration_re_findings.md`](windows_resident_icon_enumeration_re_findings.md)
(§1 repo walk, §5b CSFile load lever, §5c gfx binding, §5d residency manager). This is the TL;DR +
what's shippable + the one remaining lever.

## What we set out to do
Get real per-item ER icons onto map markers WITHOUT the player browsing each item ("tout du jeu, pas
tout du menu").

## What we SHIPPED this session (all committed, branch `docs/worldmap-projection-re-brief`)
- **Resident-icon enumeration SOLVED + validated** — the repo is an MSVC `std::map<DLWString,
  CSTextureImage*>` at `*(er+0x3d82510)+0x80`; `harvest_repo_icons()` walks it (commit `f8a0d4a`).
- **Format fix** — read DXGI format via `ID3D12Resource::GetDesc()` at copy time, not RPM pre-bind
  (`3a2ef67`).
- **BC 4-block snap + UV crop** — fixed "131 harvested, 3 drawn" (270×270 cells not %4) (`650c0f8`).
- **Throttle fix** — repo-walk re-walks on ANY `_Mysize` change (game evicts+loads → oscillates), so
  browse-to-fill accumulates the UNION into our permanent cache (`db57c9a`).
- **Deliverables** — `tools/cheat_engine/MapForGoblins_icon_repo.CT`, `D:\ghidra_scripts\walk_icon_repo.py`,
  `walk_both_maps.py`, `monitor_icon_load.py`, `monitor_icon_union.py`, `force_load_file()` test harness.

## The hard conclusion (proven, not guessed)
Runtime CAN'T cheaply give per-item icons for the WHOLE game:
- **No global page** — the game STREAMS (~150 item-icon window; `_Mysize` oscillates). repo+0x80 is the
  right map (twin repo+0xb0 has 0 icons).
- **Force-load (CSFile, `er+0x1f5560`)** loads a TPF (atlas pixels) but the per-icon **rects come from
  the sblytbnd via the gfx BINDING** (§5c), which is **virtual / menu-applied** (vtables `0x493c8f0`,
  `0x378bf40`) — not a standalone call.
- **Residency is governed by the menu resource manager** (§5d, queues `+0x1df0`/`+0x1e08`, flag
  `+0x1e19`), reached via the task system — not a flat global.

So: **runtime covers what's BROWSED** (own inventory + visited shops; our cache is permanent, write-only).
**Full coverage without browsing = offline** (rects in sblytbnd, pixels in TPF — `tools/extract_subtextures.py`
already parses sblytbnd). The map's 63 WorldMapPoint category pins need no force-load (in the world-map
gfx, resident when the map is open) + the baked category atlas (`goblin_overlay_icons`, NOT deletable —
it's the always-available base for markers + the F1 config UI; the GPU harvest is an override on top).

## The ONE remaining runtime lever (untried, the user's idea)
Hook a ticker (`FUN_140d724c0`, grab `*param_2` = the manager), then **block the unload/evict queue**
(`+0x1e08` / neuter `FUN_140d70940`'s evict) so the repo ACCUMULATES the browse union instead of
evicting → our walk would harvest the full session-union resident at once. Risky (VRAM, engine
assumptions), gated like the force-load test. Test: does the repo grow unbounded + does anything break.

## Decision pending (next session)
1. **Offline per-item bake** (robust, 100%, mod-aware) — extend `extract_subtextures.py` to emit
   `iconId → (atlas, rect)`; keep the runtime repo-walk as freshness override. ← recommended for full
   coverage.
2. **Try the evict-block lever** (runtime, accumulate the browse union) — experimental.
3. **Stop** — browse-to-fill (widened by the throttle fix) + category pins already cover the real need.
