# Bugs

Complex bugs — resolved and open — with the durable root-cause/fix takeaway. Status reflects the
**current code**, verified during the 2026-06-29 reorg. Open items are the real backlog.

## Resolved
- **Boss markers were ERR-only** [PARTIAL 2026-07-23] — bosses + Great Runes vanished on vanilla/
  randomizer/any non-ERR game: `build_live_bosses` seeded boss TYPES only from ERR's `textId2==5100`
  WorldMapPointParam pins. Now also seeds mod-agnostically from the tier-3 field-boss NpcName band
  (`enemy_display_name(...,&tier)==3`) with `Marker::live_name` + a synthetic `name_id`. Bosses now show
  WITH names on any mod — BUT tier-3 is a name-source tier, not an "is-boss" signal → dups/wrong names.
  **RESOLVED 2026-07-27**: sourced from the game's own boss health bar (EMEVD `2003[11]`, tier 4;
  `docs/re/cross_mod_boss_naming_re_findings.md`). Live: **1197 → 215 markers**, biggest type 269 → 3.
  → [mod-agnostic-boss-markers](mod-agnostic-boss-markers.md)
- **Legacy-fold grid wrapped on negative coords** [resolved 2026-07-27] — `cgx = (uint8_t)(wx/256.0)`
  wraps to 255 for a negative wx, breaking the fold chain after one hop and dumping the marker at the
  map's far corner. 19 Ashen-Capital markers; 2864/9828 m35 placements in simulation, exactly those with
  a local coord under -256. **The prior Class-C `reverse_lookup` diagnosis was wrong** — area 35 has a
  forward row and folds fine once clamped. → [legacy-fold-negative-grid-wrap](legacy-fold-negative-grid-wrap.md)
- **MSB part name is not the enemy's model** [resolved 2026-07-27] — an enemy randomizer swaps
  `ModelName`+`NPCParamID` and KEEPS the part name, so the `c####_` prefix names the creature that used
  to be there (**84.7 %** of placements diverge on Randomizer v0.11.4). Every model-keyed name lookup
  (codex tier 2, boss band tier 3) read the wrong creature; use `DiskEnemy::modelName` /
  `enemy_model_id()`. → [msb-enemy-model-vs-partname](msb-enemy-model-vs-partname.md)
- **MapForGoblins killed other mods' overlays** [resolved 2026-07-26, in-game retest pending] — three
  independent causes: (1) the raw-input / DirectInput8 / cursor hooks blanked input for EVERY caller in the
  process, not just the game (other overlays went unresponsive with F1 open) → now gated on
  `goblin::caller_is_game(_ReturnAddress())`; (2) `MH_Uninitialize()` restored pristine prologue bytes,
  erasing any mod that hooked the same slot after us; (3) `uninstall_wndproc_hook` restored the WndProc
  unconditionally, dropping later subclassers out of the chain. Same defects existed in ER-DeathCounter-Mod.
  Adds a Present hook-chain diagnostic (target/detour/trampoline/owner) to both mods.
  → [multimod-hook-coexistence](multimod-hook-coexistence.md)
- **Overlay input hooks fired while game unfocused** [resolved 2026-07-23] — F1 open + alt-tab left the
  cursor/raw-input/wndproc swallow hooks commandeering input for another app. Re-gated the INPUT path on OS
  focus (`input_capture_active() = (menu_open||vmap_covers_map) && has_focus()`) while keeping drawing
  focus-independent — doesn't regress the 2026-07-01 Alt+Tab cursor fix (event-driven `has_focus`, not the
  old flapping GetForegroundWindow poll). → [overlay-input-unfocused-hooks](overlay-input-unfocused-hooks.md)
- **vmap grace warp used the param row key, not bonfireEntityId** [resolved 2026-07-04] — double-click
  warp → infinite load (area 61/DLC stall); RPC warp worked. `warp::to_grace` needs the bonfire ENTITY
  id (`BonfireWarpParam.bonfireEntityId` @0x08, e.g. 1042362951=The First Step), but the grace layer
  used the param ROW KEY, which ERR REMAPS (61423601). Guardrail: row key ≠ warp/entity id under a mod.
  → [vmap-grace-warp-entity-id](vmap-grace-warp-entity-id.md)
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
- **Endless map pan after F1 close at screen edge** [resolved 2026-07-02] — cursor left in ER's map
  edge-pan band on close; nudged inward on the close falling edge (+ SendInput jiggle so the game
  adopts it). Measured band: ~150px @1080p with falloff → margin = height/6. → [f1-close-edge-pan](f1-close-edge-pan.md)
- **`require_map_fragments` leak** [resolved] — interior overworld tiles inherit majority fragment of
  8 neighbours (`goblin_logic.cpp:28-53`). Far-from-coverage tiles still leak (low-pri). → [fragment-gate-maplist-gap](fragment-gate-maplist-gap.md)
- **DummyAsset over-emission** [resolved] — disk walk drops MSBE part-type 9 unless entity-bound;
  21 benign residual accepted. → [msbe-dummyasset-filter](msbe-dummyasset-filter.md)
- **DLC loot-flag drop** [resolved] — `>= 0x40000000` cut caught one-time DLC flags; replaced by live
  `EventFlagMan` group-allocation query. → [resolve-loot-flag-dlc-bug](resolve-loot-flag-dlc-bug.md)
- **Disk-parser coverage gaps** [resolved] — shared `emit_lot_siblings()` across all three passes;
  EMEVD-semantics-first lesson (the 2009 "asset-lot" pass was actually "Register Ladder", reverted).
  Also: **cross-tile LOD award-entity mis-tiled off-map** [FIXED 2026-07-23] — `load_lod_award_entities`
  stamped the LOD file tile instead of the enemy part-name prefix, dropping a Teardrop Scarab's
  White Shadow's Lure at off-map grid(24,28) (fixed → Mountaintops grid(48,56)). → [disk-parser-coverage-gaps](disk-parser-coverage-gaps.md)
- **Player-position pointer chain** [resolved] — static RE wrong twice; runtime-confirmed chain. → [player-pos-static-unreliable](player-pos-static-unreliable.md)
- **Shutdown crash noise** [resolved/triage] — `eldenring.exe +0x1EB9999` teardown crash is ER's own,
  not ours; only investigate when `fault_module` is MapForGoblins.dll. → [er-shutdown-crash-noise](er-shutdown-crash-noise.md)

## Open
- **Off-map markers: ERR area 45 has no dungeon→overworld anchor** [open, root-caused] — dungeon area 45
  (ERR-added content, name_ids `900xxx`) is absent from `data/dungeon_to_world.json` (anchors only for
  10/11/12/14/16/30/31/32/34/39) → its markers keep block-local coords and pile near the origin. Found via
  the vmap marker extractor (`near0` flag). Fix = add an `m45_XX` anchor (needs the area's overworld
  entrance coords). → [offmap-area45-missing-anchor](offmap-area45-missing-anchor.md)
- **Warp to a non-grace id strands the player at (0,0,0)** [open, low — recoverable] — `warp <id>` with an
  id that isn't a real/unlocked grace teleports to a void cell instead of no-op (`ok` ≠ valid dest). Fixed
  by a valid warp. id-validation NOT feasible yet: the working warp id `1042362951` matches neither the
  captured `rowId` nor `bonfireEntityId` of any live grace (contradicts [[vmap-grace-warp-entity-id]]) —
  needs RE of the true warp-id↔BonfireWarpParam field. → [warp-invalid-id-strands-player](warp-invalid-id-strands-player.md)
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
- **Enemy names coupled to the minimap** [FIXED 2026-07-06] — `draw_enemy_bar_names` is called from
  `draw_minimap_hud`, which was gated `if (minimap)` in `goblin_overlay.cpp` — so enemy names only drew
  when `show_minimap` was on. Looked "Linux works / Windows doesn't" purely because the two boxes' inis
  differ (`show_minimap=false` on Windows). Fix: call the HUD `if (minimap || config::enemyNames)` — the
  minimap self-gates internally (`draw_minimap` checks `showMinimap`, `map_renderer.cpp`). Diagnostic:
  the `[ENEMYBAR] resolve …` log line only prints when the draw runs; the sigs (CSFeMan/WCM/GetChrIns)
  were fine all along. Host-side → needs a game restart.
- **Underground NPC/merchant pins off-map** [FIXED 2026-07-06] — the `entity_world_pos()` cache
  (`g_entity_pos`, `map_entry_layer.cpp` ~2447/2454) was built with `marker_world_pos(...)` at the DEFAULT
  `conv_underground=false`, while EVERY other world-pos consumer (grace_layer / custom_markers / map_renderer
  region pass) passes `true`. With `false`, base-underground (area 12) rows are left in area-12-NATIVE coords
  (not unified to overworld map-space). QuestNpcLayer draws these pins by WORLD coords (`m.worldX/Z` via the
  overworld affine), NOT `project()`, so the native coords landed OFF-MAP — the "6 underground merchants
  mispositioned" report (Hermit/Abandoned/Imprisoned Merchant in Siofra/Ainsel). Fix: pass
  `conv_underground=true` in both cache-build calls; `marker_group_from` keys the UG bit off the ORIGINAL
  area (12), so the pins stay on the UG layer, only the position unifies. Verified live: `vmap offmap` → 0
  off-map of 438; all `g1(UG)` merchant pins `onmap` with unified ~9k–13k coords. Render-side → hot-reloads.
- **Atlas-upload GPU-init race** [HARDENED 2026-07-23] — cold-boot crash (`fault_base +0x0`) in
  `upload_rgba` ← `try_upload_atlas` ← `hk_present`. Root cause: gate checked 3 of the 5 D3D12 globals
  upload_rgba derefs (`g_command_list`/`g_frames[0].allocator` unguarded) + `cleanup_imgui_device` left
  `g_imgui_init` stale. Fix: full readiness gate + retry-not-give-up + reset `g_imgui_init` on cleanup +
  entry null-guard. Residual (unproven): a boot device-reset could dangle the cached command queue (survives
  null guards). → [atlas-upload-gpu-race](atlas-upload-gpu-race.md)
- **Boot crash inside d3d11.dll** [open lead 2026-07-27] — intermittent `0xC0000005` ~4 s after launch,
  faulting module `d3d11.dll`, our `hk_present` in the stack scan. **We never touch D3D11** (link
  `d3d12`+`dxgi` only) → the faulting code is another overlay's; six hooking DLLs were loaded. NOT the
  atlas race (that one is a null deref). Disabling RTSS stopped it, on a small sample — a lead, not a
  verdict. Also records the mismatched-PDB symbolization trap.
  → [boot-crash-present-chain-d3d11](boot-crash-present-chain-d3d11.md)
