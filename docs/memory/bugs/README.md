# Bugs

Complex bugs — resolved and open — with the durable root-cause/fix takeaway. Status reflects the
**current code**, verified during the 2026-06-29 reorg. Open items are the real backlog.

## Resolved
- **Overlay input-hook freeze** [resolved 2026-06-30] — a `ShowCursor` detour that swallowed the game's
  hide with a constant `>=0` return made ER's `while(ShowCursor(FALSE)>=0)` loop spin forever on the game
  thread; the overlay (present thread) kept rendering → "game frozen, DLL alive". Reverted. Guardrail:
  input-API detours run on the game thread and must never loop/block or trap a game spin-loop. Open watch:
  confirm no Fullscreen freeze recurs (now Borderless + button-polling). → [overlay-input-hook-freeze](overlay-input-hook-freeze.md)
- **Item-stack toggle rebuild race** [resolved 2026-06-30] — toggling `stack_identical_items` re-kicked
  a bucket build without waiting for the previous worker → two threads mutating `g_buckets` / a shared
  `unordered_map` → AV in rehash (`crash_320`, `+0x6B265`). Now serialized to one worker + pending flag.
  → [item-stack-toggle-rebuild-race](item-stack-toggle-rebuild-race.md)
- **Map-open freeze** [resolved 100%] — **fixed by switching to the ImGui/DX overlay backend.** Markers
  are now drawn by our own overlay instead of being injected as native `WorldMapPointParam` rows, so the
  engine no longer walks any extra on-page rows at map open and the multi-second freeze is gone entirely.
  The earlier `areaNo=99` row eviction + clustering was the *pre-overlay* mitigation and is moot for this
  path. → [mapforgoblins-map-open-freeze](mapforgoblins-map-open-freeze.md) · RE detail: [thread8-mapopen-bottleneck-re](thread8-mapopen-bottleneck-re.md)
- **Clustering live-test bugs** [resolved; 1 race open] — location-anchored clusters, live re-plan on
  toggle; phantom/oversized stale-icon race on disable→enable→reopen still open. → [cluster-redesign-bugs](cluster-redesign-bugs.md) · [cluster-runtime-queue](cluster-runtime-queue.md)
- **Proton collected-refresh stutter** [resolved; sub-item open] — RPM-to-self flood on wineserver;
  fixed by bulk + in-process `__try` noinline reads (`85cece4`). **Open sub-item: graying should test
  bit7 (0x80 of +0x263), not bit1 — unapplied.** → [collected-refresh-proton-perf](collected-refresh-proton-perf.md) · [collected-geof-bruteforce-scan](collected-geof-bruteforce-scan.md)
- **Gamepad / mouse-still map drift** [resolved] — projection used the reticle field as view centre;
  re-centred on cursor-independent `(pan+snapMid)/zoom` + canvas scale. → [overlay-gamepad-cursor-bugs](overlay-gamepad-cursor-bugs.md)
- **Page-transition flicker** [resolved] — stale frame from our own view-delay ring; `g_view_delay.reset()`
  on page-group change. → [page-transition-flicker](page-transition-flicker.md)
- **`require_map_fragments` leak** [resolved] — interior overworld tiles inherit majority fragment of
  8 neighbours (`goblin_logic.cpp:28-53`). Far-from-coverage tiles still leak (low-pri). → [fragment-gate-maplist-gap](fragment-gate-maplist-gap.md)
- **DummyAsset over-emission** [resolved] — disk walk drops MSBE part-type 9 unless entity-bound;
  21 benign residual accepted. → [msbe-dummyasset-filter](msbe-dummyasset-filter.md)
- **DLC loot-flag drop** [resolved] — `>= 0x40000000` cut caught one-time DLC flags; replaced by live
  `EventFlagMan` group-allocation query. → [resolve-loot-flag-dlc-bug](resolve-loot-flag-dlc-bug.md)
- **Disk-parser coverage gaps** [resolved] — shared `emit_lot_siblings()` across all three passes;
  EMEVD-semantics-first lesson (the 2009 "asset-lot" pass was actually "Register Ladder", reverted). → [disk-parser-coverage-gaps](disk-parser-coverage-gaps.md)
- **Player-position pointer chain** [resolved] — static RE wrong twice; runtime-confirmed chain. → [player-pos-static-unreliable](player-pos-static-unreliable.md)
- **Shutdown crash noise** [resolved/triage] — `eldenring.exe +0x1EB9999` teardown crash is ER's own,
  not ours; only investigate when `fault_module` is MapForGoblins.dll. → [er-shutdown-crash-noise](er-shutdown-crash-noise.md)

## Open
- **Native-row live refresh** [open, minor] — our overlay markers refresh live every frame, but the
  legacy native section/grace-pin toggles that flip `areaNo` still only apply on map reopen (needs
  CSWorldMapPointMan rebuild RE). Does **not** affect the overlay marker path. → [thread8-mapopen-bottleneck-re](thread8-mapopen-bottleneck-re.md)
- **Render-loop perf** [open] — ~8477-marker/frame loop; idle-skip + spatial bucketing are backlog
  (see `process/plan-spatial-grid-audit`). → [overlay-render-perf-followups](overlay-render-perf-followups.md)
- **DX/bugs backlog** [open, mostly stale — audited 2026-07-01] — real remaining items: 4/5 (in-game
  pause, RE spike not started), 13/14 (minimap doesn't honour marker-scale/clustering, no search-ring),
  16 (native ER right-stick zoom bug), F1 (native overworld icons leak underground after Browser
  teleport), F2 (locate pan clamped at fog-of-war boundary). Items 1/3/6/7/8/9/10/12/15 already FIXED
  (cross-checked against `git log` this session — the doc had drifted); 2 partial (cursor-recenter
  done, key-hint auto-switch not); 11 root-caused as a double-DLL-load deploy issue, not code (hardening
  guard still TODO). → [dx-bugs-backlog](dx-bugs-backlog.md)
- **Overlay double-draw** [root-caused, not a code bug] — was thought to be a double Present-hook/init;
  actually the double-DLL-load artifact (`MapForGoblins.dll` + `MapForGoblins_vanilla.dll` both
  loading), see `docs/HANDOFF.md` "Known bugs". Practical fix = deploy one DLL; a runtime mutex-guard
  hardening is still TODO. → [dx-bugs-backlog](dx-bugs-backlog.md) (item 11)
- **Phantom cut graces** [open] — Siofra nameless + "Underground's End"; needs MSB entityId allowlist
  (Oodle-blocked on Linux). → [extra-graces-siofra](extra-graces-siofra.md)
- **Per-tile walk-fog** [open RE] — real explored-fog lives in `CS::WorldMapTiledLayer`; `tile_fogged()`
  third gate unbuilt (needs Ghidra). The redundant piece-flag gate was removed. → [worldmap-tile-fog-re](worldmap-tile-fog-re.md)
