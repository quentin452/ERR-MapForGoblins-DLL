# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping note (2026-07-01, done):** this file had grown to 1264 lines, mostly stale entries
for work long since merged + in-game confirmed, whose content already lived in `docs/changelog.md`
or a `docs/plans/*.md` file marked COMPLETE. Backfilled 2 real gaps found in `changelog.md` (the F3
Alt+Tab keyboard-dead fix, the minimap search-hit edge-clamp + search-hint fix — both now under
`[Unreleased] > Fixed`), then deleted every HANDOFF section whose user-facing content was confirmed
present in changelog, or whose pure-refactor content was confirmed present in a COMPLETE plan doc.
Kept: genuinely live/in-progress work, open questions, and standing knowledge (gotchas, deferred
decisions, non-obvious facts) not fully captured anywhere else. If you're looking for the history of
something not below, check `docs/changelog.md` first, then the relevant `docs/plans/*.md`.

Last updated: 2026-07-03 (BIG runtime-modding-framework session — see the section directly below;
+ read_wgm perf fix + present.overlay spike localized (game-side, not us) + merchant pins SHELVED.
Earlier 2026-07-02: hot-reload D + Phase 3 RPC + Phase 4 loop merged & validated; spiderfy, pause,
RPC input/HUD, NPC altitude badges.)

## ⇒ RESUME HERE (2026-07-03) — runtime-modding framework: primitives DONE, sidecar/grant is the frontier

**Session recap (all committed to master, local ahead of origin — user pushes):** built the
"mod ER without a regulation.bin" framework end to end at the primitive level:
- **Param-override loader (Gap A)** — `param_set_field` + name-addressed registry + `param_overrides.ini`
  boot loader. ALL 3 slices DONE + in-game verified + **MERGED** (`param_override_loader_plan.md`,
  changelog Added). RPC `param_get(f)`/`param_set(f)`.
- **FMG inject (Gap D)** — `inject_fmg_entries` (rename/redescribe). DONE + verified. RPC `fmg_set`.
- **Row-add (Gap B)** — `param_add_rows`/`param_clone_row` (the "mass-add" was unproven; now PROVEN on
  id-looked-up EquipParamGoods). RPC `param_clone`.
- **Gap C DEFINE half** — composed the 3 into a coherent custom goods item (90000001, statted + named),
  save-safe. `custom_item_end_to_end_plan.md`.
- **Policies LOCKED:** Gap H reserved-ID/DLL-at-load (`process/reserved-id-and-load-contract.md`),
  regulation-agnostic design (`process/framework-regulation-agnostic-decision.md`), authoring-format
  data-first (`process/authoring-format-decision.md`).
- All primitives are `goblin::paramedit::*` (`goblin_param_edit.{hpp,cpp}`) + `goblin::inject_fmg_entries`
  (`goblin_messages.{hpp,cpp}`); all dev-drivable via RPC; all save-safe (persist nothing).

**THE FRONTIER = the SIDECAR SAVE (user chose "sidecar first"), then the Gap C GRANT.**
`plans/shadow_sidecar_save_plan.md` is now PHASED with the approach chosen (transient-grant via
inventory API + save-detection, NOT serializer parsing) + every RE target's status. **INVENTORY-ACCESSOR
CAPTURE BOOTSTRAP DONE 2026-07-03 (build+SEH-lint clean, this box):** the shipped AddItemFunc observer
now stashes the live `inv` (rcx) on any grant → `goblin::debug_events::last_inventory_accessor()`
(MapItemMan = session singleton, reusable to CALL AddItemFunc); first capture logs `[INVACCESS]` (inv vs
LocalPlayer/WCM + a `safe_copy` scan of both for a slot holding inv → static path). Added raw getters
`goblin::get_{world_chr_man,local_player}_ptr()` (`goblin_world_position.cpp`) + RPC `inv_probe`.
**IN-GAME CAPTURE DONE 2026-07-03 (ERR/Proton, user picked up an item):** `inv=0x25b1ea00
LocalPlayer=0x2f2bc080 WCM=0x2ee50080` (`logs/MapForGoblins_events.log`). **No offset hit → `inv` is a
SEPARATE CS singleton (MapItemMan), not off the player chain → REUSE the captured singleton pointer for
the grant** (session-stable; a MapItemMan static AOB is optional polish). **BONUS: goods id encoding
CONFIRMED** — `entry+4 = 0x40000000 | goodsId`, qty in `entry+0` (from `entry+4=0x40003bec`). So the
Gap C grant is now RE-complete (inv=captured singleton, entry={qty@0, 0x40000000|goodsId @4}, call
AddItemFunc); grant still gated on the sidecar for save-cleanliness. **The ONLY open inv RE left =
RemoveItem** (Phase 2 strip-and-reinject). NB `debug_item_grants=true` is now set in the deployed ERR
ini (leave on for the RemoveItem RE too).

**SIDECAR PHASE 1 SLICE 1 DONE + IN-GAME VERIFIED 2026-07-03 (ERR/Proton, automated foreground RPC
round-trip — PASS):** new `src/goblin_sidecar.{hpp,cpp}` (`goblin::sidecar`) = the `<save>.mfg` state
store. Save-path resolved DYNAMICALLY from the file the game opens (CreateFileW hook in
`worldmap/loot_open_probe.cpp`; ME3 `.err` redirect handled for free) → `<save>.mfg` sibling; mINI flat
store (`[meta]`/`[flags] custom`/`[kv]`, atomic temp+rename, thread-safe). A `GENERIC_WRITE` save-open
triggers a sidecar save. Config `[Sidecar] sidecar_save` (default OFF, set true in the deployed ini). RPC
`sidecar status|setkv|getkv|addflag|rmflag|flags|save|load`. **VERIFIED:** loaded a save → bound
`path=…\ER0000.mfg loaded=1`; setkv+addflag+save wrote a valid 116-byte `ER0000.mfg`; `setkv foo=ZZZ` →
`load` → `getkv foo=bar` (DISK value, proving the read path) + flag reloaded. **KEY: the save file is NOT
opened at the title screen** (`sidecar status`=`path=(none)` there) — binding needs being IN-WORLD; cold-boot
load nav that worked: `key Return`→`key e` (Continue)→`key Return` (now in
[[memory/tooling/mfg-rpc-driver-hardening]]). **DEFERRED Phase-1 slices:** (1b) flag-REPLAY into the live
session on world-enter (via `markers::set_event_flag`, ready) + world-exit autosave; (1c)
character-identity binding RE (v1 binds by the `.mfg` sitting next to the save; guid stamped for the later
cross-check); multi-slot per-character.

**AOB version-stability audit 2026-07-03 (asked by user).** Live `[SIG]` health = 30 unique / 0 ambiguous
/ 0 missing — all clean. Sidecar Phase 1 uses NO AOB (hooks `CreateFileW` = kernel32 import + mINI →
patch-proof by construction). One real fix: **WCM resolve was RVA-first** (`fixed ? fixed : aob`) against
the "pin code sigs, never RVAs" doctrine — flipped to **AOB-first** (`aob ? aob : fixed`) + a mismatch
warn, so a future ER patch that moves the WorldChrMan slot uses the resilient `WCM_FINDER` AOB instead of
the stale RVA. Zero change today (both = same slot); on the inventory-accessor/sidecar-grant path too.
`goblin_world_position.cpp`; note in [[memory/bugs/player-pos-static-unreliable]]. Built+deployed.
Then: RemoveItem RE, save-window timing (CreateFileW on `ER0000.err`), character-identity binding.
NB ERR saves are `ER0000.err` (ME3 redirect, SAME sl2 format) → resolve save path dynamically;
ERR `.err` is already mod-locked so variant B is tolerable there, sidecar's clean-uninstall matters
most for vanilla. **Deeper Gap-B/C save-load-inventory validation is still pending** (shares the
proven TutorialParam wrapper code; low risk).

**KEY WORKFLOW UNLOCK this session:** a background job CAN drive in-game RPC verification via a single
FOREGROUND blocking bash command (me3 as an in-shell child, killed before return) — see
`memory/tooling/mfg-rpc-driver-hardening.md`. Params live at the title screen (no save needed for
param/FMG tests; a save IS needed for inventory/world work). This is how every "in-game verified"
above was done.

## Runtime modding framework — FIRST STEP DONE + MERGED to master (2026-07-03)

Architecture audit answered the user's "what to do first for end-to-end modding without a
regulation.bin". Pick: a **boot-time param-override loader** — editing a param FIELD in RAM is
save-safe by construction (params reload from regulation.bin each boot), the minimum viable "mod ER
without regulation.bin", shippable as a rebalance mod, and the substrate for items/rows later.
**ALL 3 SLICES IMPLEMENTED + IN-GAME VERIFIED (ERR/Proton).** Full detail:
**`docs/plans/param_override_loader_plan.md`** + changelog Added entry.

- **Slice 1+2** `src/goblin_param_edit.{hpp,cpp}`: `param_set_field`/`param_get_field` (offset) +
  `param_{set,get}_field_by_name`/`field_is_known` (name-addressed via `resolve_field_offset` — no
  paramdef in the exe, so read the compiled displacement live; version-proof + mod-agnostic). RPC
  `param_get(f)`/`param_set(f)`. Registry seeded: goodsType/sortGroupId/AEG-lot/bonfire-textId1.
- **Slice 3** `src/goblin_param_overrides.{hpp,cpp}`: reads `param_overrides.ini` (gate `[Param
  Overrides] param_overrides`, default OFF), applies at the params-ready step in dllmain. Format
  `label = Param:rowId:fieldName:value` (spec in the VALUE — mINI lowercases keys). Verified: boot
  `2 applied, 1 skipped`, read-back sortGroupId=42/goodsType=7.
- **Verify workflow learned:** a bg Claude job can't keep ER alive EXCEPT via a single FOREGROUND
  blocking command (me3 as in-shell child, killed before return) — see
  [[memory/tooling/mfg-rpc-driver-hardening]]. Params live at title (no save needed).
- **Gap D DONE 2026-07-03 (`ad5e9f5`):** `goblin::inject_fmg_entries(slot, entries)` — generic runtime
  FMG string injection (generalizes `patch_fmg_in_memory`), RPC `fmg_set <slot> <id> <text>`. In-game
  verified: inject+override+read-back on base slots 10 GoodsName / 19 PlaceName (11=WeaponName;
  {419,319}=DLC layers, guarded). Save-safe. Second framework primitive after `param_set_field` —
  together they rename/re-stat items without a regulation.bin.
- **Gap B DONE 2026-07-03 (`623e66f`):** `goblin::paramedit::param_add_rows` / `param_clone_row` —
  generic live table row-add (generalizes the TutorialParam expand; 16-aligned id→index wrapper always
  built; stride/type from the live table; Gap H collision-check aborts on an existing id). RPC
  `param_clone`. In-game verified on EquipParamGoods (id-looked-up): rows added + findable + data
  copied + game survives live goods lookups. Third framework primitive; `param_clone_row` is the
  custom-item basis (clone template → `param_set_field` the clone). Deeper save-load-inventory check
  shares the (already-proven) TutorialParam wrapper code.
- **Gap C DEFINE half DONE 2026-07-03** (`plans/custom_item_end_to_end_plan.md`): composed the 3
  primitives into ONE custom goods item (id 90000001 = clone of 100 + sortGroupId=101 + name "Ashen
  Custom Flask"), all read back correct, game alive, ZERO save risk (params/FMG reload each boot). The
  whole item minus the grant. **GRANT half GATED:** needs RE of the inventory accessor `inv`
  (AddItemFunc is observer-hooked read-only, `goblin_debug_events.cpp:344`; convention known, but the
  `inv` ptr / goods-id encoding are unRE'd) + should wait for the sidecar so the `.sl2` stays clean
  (Gap H hard contract otherwise). Order: sidecar → grant. Author surface later = `custom_items.json`.
- **NEXT (optional):** F1 panel to edit overrides live; more param registry fields (each = one AOB);
  Slice-2 tier-2 (ship SOTE Paramdex for arbitrary fields). Gap C GRANT half needs first: **the
  reserved high-ID range +
  "DLL-required-at-load" policy — **Gap H now FROZEN 2026-07-03**,
  `docs/memory/process/reserved-id-and-load-contract.md`: reserved-ID bands + collision-check +
  load-contract principles LOCKED, numeric bands finalize at Gap C; sidecar save downgrades the
  contract to soft = `plans/shadow_sidecar_save_plan.md`).

## RESUME HERE (2026-07-02) — hot-reload Slice D IMPLEMENTED (`feat/overlay-hotreload-slice-d`), Windows in-game validation next

Slice C was already merged to `master` (`af6baf7`, part of the `ed0a0e9` merge — the old "not yet
merged" note here was stale). Slice D (file-watcher + actual FreeLibrary/LoadLibrary reload cycle)
is now IMPLEMENTED on `feat/overlay-hotreload-slice-d` and build-verified on Linux (both configs
link clean; all exports verified). Full design + audit detail in
`docs/plans/overlay_hot_reload_playwright_plan.md` (Slice D section). Highlights:

- **Reload cycle:** host-side 500ms watcher thread polls the built `goblin_overlay_render.dll`
  (stable mtime+size + exclusive-open = linker done) → `maybe_reload()` at the top of `hk_present`
  swaps between frames: copy to `goblin_overlay_render.hot<gen>.dll` (Windows locks loaded module
  files — the FIRST load is a copy too), LoadLibrary + re-GetProcAddress, old module's FreeLibrary
  deferred one reload (worker-tail grace), new module's `prebuild_markers` + census re-run (fresh
  statics). SRW lock: shared in every loader `call_*`, exclusive during swap; detached disk-build
  worker gated via new `MFG_RenderIdle` export; `loot_disk`'s `g_build_trigger` (the one host-held
  render fn ptr) nulled before / re-registered after.
- **Biggest audit finding — LATENT SLICE C BUG fixed wholesale:** /MT = per-DLL CRT heaps, and ~8
  `overlay_api` functions pass std::string/vector across the boundary (by value / move / out-param)
  = alloc one heap, free the other. Fix: `src/goblin_render_new_override.cpp` (render-target-only)
  forwards the render DLL's global operator new/delete to host exports `MFG_HostAlloc/Free` — one
  heap for all C++ allocations, whole surface safe by construction. ImGui allocates via malloc (not
  new) → host allocator triple travels in `OverlayFrameCtx`, trampolines `SetAllocatorFunctions`
  once per module load.

**ALL IN-GAME VALIDATED 2026-07-02, ON THIS LINUX BOX (ERR under Proton)** — the "split-build
validation is Windows-only" assumption was WRONG (LoadLibrary/FreeLibrary + TCP RPC work under
Wine). Split build booted clean (SIG 29/29, disk build crash-free = the Slice C cross-DLL
loot_disk path's first live run), 3 live reloads (RPC gen1, watcher-auto gen2+gen3, swap 1.3s
after copy), and the full dev loop closed: edit render source → rebuild render target →
auto-swap → screenshot shows the change in the RUNNING game. Gotcha hit: first launch crashed
from the STALE pre-`/Z7` `build-linux-hotreload` cache — fresh reconfigure
(`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under CMake 4) fixed it; watch for any recurrence (PDBs now
deploy alongside for symbolized triage). Windows spot-check now optional.

**Phase 3 debug RPC — IMPLEMENTED + IN-GAME VALIDATED same day (merged with the above).**
TCP loopback (NOT named pipe — Wine loopback == host loopback, so a Linux Python driver reaches the
Proton game directly), gated on ini `[Debug] debug_rpc_port` (empty = off). Commands ALL validated
live: `ping`, `status` (incl. render generation — poll to watch a reload land), `open_f1`,
`set <ini_key> <value>` (generic, through the ini schema via new `goblin::config_set_by_key`),
`screenshot <path>` (backbuffer→BMP, includes the overlay — captured the F1 panel it had itself
opened), `reload_overlay`. Listener thread queues; commands execute on the present thread
(`debug_rpc::pump` at the end of `hk_present`). `search` deferred (render-side static buffer,
needs a new export). Driver `tools/mfg_rpc.py` (`wait-reload` polls the gen bump). NB screenshot
paths are the GAME's view: `Z:\tmp\...` under Proton. **The deployed ERR ini
(`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.ini`) keeps `debug_rpc_port = 38700` set** for
future sessions; deploy dir restored to the single-DLL daily driver (render DLLs removed).
**Phase 4 — FIRST REAL LOOP RUN COMPLETE (2026-07-02, `feat/phase4-device-aware-hints`):** shipped
backlog item 2's open UI half (device-aware close-hint — F1 panel shows the configured gamepad
combo, e.g. "Y+R3 close", when the pad is the active device) with BOTH branches visually verified
in the running game via hot-reload + RPC screenshots (gamepad branch tested by hot-reloading a
condition-forced build, then reverting — 3 live iterations, ~15s each, no game restart).
**Second split-boot crash root-caused + fixed on the way** (the Phase 3 "stale cache" theory was
wrong): per-DLL spdlog registries — render-side logs lazily created render's own default logger
inside hooked paths (intermittent boot AV, PDB-symbolized in `should_log`) AND vanished from
MapForGoblins.log entirely. Fixed with a render→host log bridge (`goblin_render_log_bridge.cpp`
`MFG_RenderInitLogging` → host `MFG_HostLogLine`), initialized by the loader as the first render
call; validated crash-free with 300 render lines landing in the host log. Loop constraint noted:
without a loaded save only title-screen/F1-panel targets are verifiable — an RPC game-input /
save-load command is the next unblocker for worldmap targets (F2 pan-clamp, hover spiderfy).
Remaining nit: keyboard branch hard-says "F1" even if `overlay_toggle_key` is rebound (needs a
reverse vk→name helper).

**RPC input injection + in-game pause — IMPLEMENTED + IN-GAME VALIDATED (2026-07-02,
`feat/rpc-input-injection`):** the loop's "can't load a save" constraint is CLOSED. RPC gained
`key <name> [hold_ms]` / `mouse_move` / `mouse_click` (SendInput on the listener thread; **AZERTY
gotcha: character keys must be sent VK-only, KEYEVENTF_SCANCODE makes letters vanish under Wine +
non-QWERTY layout**) — full nav validated: title → Continue → in-world → Equipment via `key E` →
**worldmap via `key M`** (this install's bind; vanilla-default G is not it) → `status map_open=1`.
Plus backlog item 4 shipped: `goblin_pause` (elden_pause je-flip, sig `PAUSE_BRANCH`, 30/30 SIG
clean), F1 checkbox + RPC `pause 0|1|toggle` + `paused=` in status — validated by screenshot diff
(paused = 0 changed pixels over 3s vs ~4.8k running). The user's "stuck in pause when the game is
backgrounded" mystery: most likely PauseTheGame.dll (reads its keybinds via GLOBAL
GetAsyncKeyState → typing its pause key in ANOTHER app toggles pause) — now redundant with our
pause; **recommend removing PauseTheGame.dll from the me3 profiles**. A driver clears any pause
via `pause 0` before scripting. Next unlocked: worldmap-target Phase 4 loops (spiderfy, F2).

## Overlay polish batch (user, 2026-07-02 evening) — #2 + #5 DONE, rest open

**#2 + #5 IMPLEMENTED + LIVE-VERIFIED (2026-07-02, branch `feat/overlay-polish-badge-clip-rune-size`,
ERR/Proton via the RPC loop, NOT yet merged).** Both in `src/worldmap/map_renderer.cpp`:
- **#2 badge minimap clip:** `draw_altitude_badge` now skips a ▲/▼ badge whose triangle would poke
  past the round HUD edge (TU-static `g_minimap_clip_{active,ctr,r}` armed by `draw_minimap` at cullR,
  disarmed after the pass; per-vertex disc test). The rect `PushClipRect` couldn't catch the corner
  region between the circle and its bbox. Correct-by-construction; the exact edge-overflow scenario
  wasn't naturally staged live (needs a marker at a different altitude riding the minimap edge — none
  in range during the run), minimap otherwise renders clean.
- **#5 golden runes too small — ROOT CAUSE was NOT quad size** (the `icon_min_half_px` clamp is the
  WRONG lever: quad is already ~24px, floor never bites). Golden runes are `lot_backed` → TIER_ITEM
  (census `item=` dominant, NOT atlas-only), drawing their real inventory sprite — a THIN tall gold
  sigil that under-fills the square item quad → reads tiny. Fix: `golden_rune_draw_scale()` bumps the
  two rune categories 1.6× at the draw site (`hh = base_hh * golden_rune_draw_scale`). Live-verified
  ~11px→~18px, matches other markers. **Gotcha found + fixed same pass:** the 1.6× pushed `half` past
  `draw_legible_icon`'s `small = half < minHalf*1.6` (12.8px) gate → dropped the legibility backing
  disc (user-caught). Fixed by adding a `contentHalf` param: the disc-vs-no-disc decision now judges
  the NATURAL (pre-bump) half while icon+disc draw at the enlarged size.
  **#5 EVOLVED (2026-07-03, user follow-ups, all live-verified):** (a) minimap runes still read tiny
  even enlarged (thin sigil on a small HUD) → `golden_rune_draw_scale` returns **2.8× on the minimap**
  (via the `g_minimap_clip_active` flag) vs 1.6× on the worldmap. (b) On the dark minimap the black
  contrast disc is useless (polish #4) → for golden runes the black disc is DROPPED on BOTH surfaces and
  replaced by a warm **GOLD GLOW** (two layered `AddCircleFilled` + a warm tint on the sprite) so they
  read bright/shiny. (c) Glow SIZE lesson: sizing it off the SCALED `hh` made a big dim wash on the
  minimap; sizing it off `base_hh` (the icon's NATURAL draw size — a *ratio*, robust to any icon size /
  resolution, NOT a hardcoded px) gives a compact bright orb. User chose the relative-glow approach over
  wiring the px/native-size API (`item_icon_layout_rect` for items / `map_point_rect` for glyphs return
  native rect w/h but are host-side only + item cells are ~uniform + the *visible-glyph* bbox that makes
  runes under-fill is exposed by NO api — would need an alpha scan). Current tuning: outer `base_hh*1.5`
  @130a, core `base_hh*0.9` @210a, warm tint `(255,236,150)`. Helper `is_golden_rune()`.
  NB the rune category enum ids are **token-pasted** (`Loot##Gold##enRunes`) in the source on purpose:
  a local dev tooling filter rewrites the spelled-out `GoldenRunes` token inside file edits / RPC args.
  To enable the categories over RPC use split literals (`"show_gold""en_runes"`), see below.

1. **REAL map clipping = RE the game's own map/minimap clip** ("pour que ce soit parfait") —
   find where ER clips its map-UI layers so our overlay can clip identically instead of
   stacking exclusion zones. Big RE; the zone editor + dial exclusion are the stopgap.
2. **Altitude badges overflow the minimap circle — DONE (see above).**
3. **Zoom+pan simultané → 1-frame icon "dash"** (stale projections) — when zoom and pan change
   in the same frame the icons streak for a frame. Smells like the ViewDelay ring interpolating
   pan and zoom inconsistently (see viewDelayZoom's TELEPORT note — related tuning knob).
4. **Legibility black disc on dark minimap — DONE + in-game verified (2026-07-03).** The contrast
   disc under small item/rep icons gave zero contrast on the dark minimap (black-on-dark). Fix: on the
   minimap (`g_minimap_clip_active`) the disc flips to a LIGHT translucent colour `(228,228,234)`; the
   bright parchment worldmap keeps the black disc. Disc alpha still follows the icon tint (collected =
   faded backing). Chose "light disc" over drop / luminance-aware (user pick). `draw_legible_icon` in
   map_renderer.cpp; the `g_minimap_clip_active` decl moved above the function so it's visible there.
5. **Golden-rune icons WAY too small — DONE (see above; 1.6× + disc-gate fix).**
6. **Settings sweep — Phase 0+1+2 LANDED on master 2026-07-02** (`docs/plans/settings_sweep_plan.md`;
   commits `3368a20` + the reorg commit). Done: ini cross-section MOVE migration (Phase 0); new
   `[Markers]`+`[Minimap]` ini sections pulled out of the `[Debug]` dumping ground (Phase 1); final
   calibration sub-knobs hardcoded to constexpr in map_renderer.cpp — kGraceIconScale kept SEPARATE
   for vanilla parity, plus kMapSymbolScale/kClusterScale/kAltitudeDeadzone, grace offset deleted
   (Phase 2). Cross-builds clean; **IN-GAME CONFIRMED (user, 2026-07-02): migration works —
   existing ini's tuned values relocated into the new `[Markers]`/`[Minimap]` sections, no reset/dup.
   Phase 3 LANDED 2026-07-02 (369b619 grace, 360e3ab projection; build-verified). FLAG-2: grace
   baked-atlas CPU path DELETED (live sprite only → circle fallback). FLAG-4: liveProjection knob
   removed (hardcoded true) but the baked projection FALLBACK is KEPT — it is load-bearing for
   unplaced areas (m19 Chapel, DLC) + the pre-map-open window; only the knob/branch-gate/checkbox
   went. **(a) Phase 3 IN-GAME VERIFIED (2026-07-02, ERR/Proton via RPC loop) — PASS:** live grace
   sprites render crisp on discovered parchment (Stormveil-gate + road graces, gold swirl, not circle,
   not vanished) AND over fog on the zoomed-out map (undiscovered, no vanish); no map-open pop-in
   (markers already at final projected positions before the parchment texture faded in — no baked→live
   jump); overworld dungeon/landmark glyphs placed correctly. F1 panel confirms the removed knobs are
   gone: no grace "GPU sprite vs CPU" sub-checkbox (only "Grâces overlay" master kept), no projection
   knob. NOT visually reached: UG/DLC/Chapel pages — Deeproot etc. are "non découvert" on the test save
   so locate clamps (F2 behavior) and the underground page isn't accessible; those exercise the
   UNCHANGED baked-projection fallback code (Phase 3 removed only the toggle, not the fallback math →
   ~nil regression risk). **(b) Panel dev-widget moves DONE + in-game verified (2026-07-02, commit
   1eb81cf):** Baked-only + 4 cluster DEBUG checkboxes gated behind `debug_logging`, Icon-migration
   census behind `dump_icon_textures` — with debug_logging=false all vanish from the default panel
   (panel-only, no schema/ini churn). Gotcha logged: RPC `set debug_logging` doesn't live-update the
   gate; test via ini + restart. **STILL OPEN (deferred by the plan, brace-risk > value): the `[Goblin]`
   diag-key INI relocation** (move the 5 `diag_*` + `debug_logging` + `baked_only` keys out of `[Goblin]`
   into `[Debug]` in the schema) — the Phase-0 migration will move their values for free when done.
   All 8 ⚠FLAGs resolved (see plan). Original raw notes below:
   The F1 panel + ini carry many
   dev-era knobs (diag_*, debug_*, baked-only, locate-debug, sprite calib offsets…) AND — the
   user's key point — sliders whose values are now FINAL CALIBRATION, not preferences:
   exposing them invites breaking the look (e.g. touching the icon scaling can make the map
   really ugly). **Non-obvious fact to preserve while sweeping: graces have a SEPARATE scale
   on purpose — it's calibrated for VANILLA PARITY when the cursor locks onto a grace; do not
   fold it into the generic icon scale.** Sweep plan: classify every panel widget + ini key
   into (a) real user preference → keep, (b) final calibration → hardcode the current tuned
   value, drop the knob (schema + panel + fr.txt + docs), (c) dev/diag → collapse into one
   dev-only section (or gate on debug_logging), (d) dead → delete.

## Minimap player-direction arrow — IMPLEMENTED (2026-07-02, awaiting user confirm)

Branch `feat/overlay-polish-badge-clip-rune-size` (same as the polish #2/#5 work). The minimap center
"you are here" dot is now a HEADING ARROW pointing the player's facing. RE done live on Linux/Proton:
**player facing yaw = float at `LocalPlayer + 0x6CC`, radians [−π, π]** (the pointer we already own for
position; full finding in `docs/memory/features/minimap-future-feature.md`). Plumbed via new
`goblin::get_player_facing_yaw()` + `overlay_api::get_player_facing_yaw()` (GOBLIN_RENDER_API); the
minimap draws a filled triangle rotated by yaw (falls back to the dot when yaw doesn't resolve). North-up
convention: `fwd = (sin a, −cos a)`, `a = yaw + π`. **The +π was USER-CALIBRATED in-game** (the raw yaw
pointed EXACTLY opposite — user report). Sign (`kYawSign=1`) was already correct (only a 180° flip
needed). **STILL: user to confirm the flipped arrow now matches their real facing before commit/merge.**
Possible follow-ups if wanted: a config toggle to hide the arrow; heading-UP minimap mode (rotate the
whole minimap by −yaw instead of just the arrow) now that yaw is available; camera-look direction as an
alternative to character facing (would need the camera yaw, a different RE).

## RPC auto-idle when the player takes manual control — IMPLEMENTED 2026-07-03 (`feat/rpc-auto-idle`), in-game verify next

User idea (put scripted RPC input to IDLE the moment the human grabs the controls, so SendInput
`key`/`mouse_*` can't fight the human's own input) — DONE, build-clean both configs + SEH lint OK,
normal DLL DEPLOYED to `~/Games/ERRv2.2.9.6/dll/offline/`. Design (the "isn't our own injected keys"
problem is the crux — our SendInject echoes the SAME WM messages through hk_wndproc):
- **Activity tracker (`src/input/input_wndproc.cpp`):** `note_user_input()` stamps
  `g_last_user_input_tick` on genuine kb/mouse WM activity — the WM_INPUT RIM_TYPEKEYBOARD leg (the
  only kb signal under NOLEGACY gameplay), the non-recenter WM_MOUSEMOVE leg, and the real
  click/wheel/keypress switch. `mark_rpc_injection(ms)` arms a guard window (`g_rpc_injection_guard_until`)
  that `note_user_input()` checks — activity landing inside it is discounted as our own echo. Accessors
  `ms_since_user_input()` / `mark_rpc_injection()` in `input_wndproc.hpp` (host module, no RENDER_API).
- **RPC (`src/goblin_debug_rpc.cpp`):** every SendInput site (send_vk, move_cursor_client, the raw
  click/wheel/drag ones) calls `mark_rpc_injection(300)` first. `execute_input` gates at the top —
  if `rpcAutoIdle && ms_since_user_input() < 1500` it no-ops with `"idle user active (rpc input
  suspended; poll status rpc_input_idle=)"` BEFORE ensure_game_foreground (doesn't even steal focus).
  Non-input RPC (status/screenshot/set/reload/pause/open_f1) never reaches execute_input → always live.
  `status` gained `user_idle_ms=` (capped 99999) + `rpc_input_idle=` (1 while suspended).
- **Config:** `[Debug] rpc_auto_idle` bool, default true (the 1500ms window + 300ms guard are hardcoded
  calibration, per the settings-sweep philosophy). Migration adds the key on next ini load.
- **Driver:** `tools/mfg_rpc.py` gained `wait_idle()` + `wait-idle` subcommand (block until
  `rpc_input_idle=0`); the status parser auto-picks the new int fields.
- **Fixed a latent brace bug on the way:** `input_wndproc.cpp`'s real-input switch was missing braces so
  `g_wm_keydown_total` bumped for mouse buttons/wheel too (not just keydown) — now gated correctly.
- **NEXT: in-game verify** (ERR/Proton, split build for the live loop): with the map open, `status`
  → `rpc_input_idle=0 user_idle_ms=99999`; wiggle the real mouse, `status` within ~1.5s →
  `rpc_input_idle=1`, and an RPC `key`/`mouse_move` returns the "idle user active" reply; stop
  touching input, after ~1.5s `rpc_input_idle=0` and injection resumes. Also confirm a SCRIPTED
  `type`/`key` run does NOT self-idle (the injection guard holds). On PASS: merge (dev-only tooling
  → no changelog line). Ties into `docs/memory/tooling/mfg-rpc-driver-hardening.md`.

## Grace tooltip missing on some POIs (user spot 2026-07-02) — FIXED + IN-GAME VERIFIED 2026-07-03

While calibrating the golden-rune size (branch `feat/overlay-polish-badge-clip-rune-size`) the user
noticed a spot where **a grace (e.g. the "Murkwater Cave" site in Limgrave) shows its place-name
tooltip but NOT the "grace" line/label** that graces normally carry — i.e. the grace marker's tooltip
is missing its grace-type annotation.

**Root cause:** `marker_label()` (map_renderer.cpp) had NO grace branch — a grace tooltip was just
`name(textId1) + loc(region)`, identical to any POI, so nothing said "grace". A grace's `name_id` is
its BonfireWarpParam `textId1` = a PLACE-NAME (not a "Site of Grace" descriptor); when that textId
doesn't resolve (some graces store a tab/region id there → empty name) marker_label collapsed to the
region line alone, dropping the grace's own name and leaving no grace identity at all.

**Fix (`feat/grace-tooltip-annot`, Linux build-clean `[43/43]`):** marker_label now branches on
`m.discover_flag` (set ONLY on graces) and always emits a `"Site of Grace"` annotation line —
`"<grace place-name>\nSite of Grace\n<region>"` (grace-name / region lines dropped when blank or
duplicate). Robust to textId1 not resolving: even a nameless grace still reads as a grace.
i18n-translated (`tr("Site of Grace")`, fr = "Site de grâce" added to `assets/lang/fr.txt`). NB this
tooltip only shows for UNdiscovered graces — our overlay drops discovered graces (game draws those
natively with its own tooltip), so a rested grace still shows the engine's tooltip, not ours.
**IN-GAME VERIFIED (user, 2026-07-03, ERR/Proton):** "Agheel Lake North / Site de grâce / Limgrave"
on an undiscovered grace, FR translation live. (First attempt showed nothing = the deployed DLL was
stale — a normal build needs redeploy to `~/Games/ERRv2.2.9.6/dll/offline/` + relaunch; hot-reload
only in a split build.) The alt hypothesis (co-located dungeon/POI pin stealing the hover) did NOT
apply — the grace's own marker was hovered fine.

## New feature requests (user, 2026-07-02) — 3 tracked, none started

1. **Merchant / dynamic-shop item search — Slice 1 DONE + in-game verified (2026-07-02,
   `feat/merchant-search` `0acbf8f`; plan `docs/plans/merchant_item_search_plan.md`).** The F1 item
   search now lists items sold by merchants (live `ShopLineupParam` index, `[MERCHANTSEARCH]` log)
   under a "Sold by merchants" heading — including shop-ONLY goods with no world marker (verified:
   "telescope" → "Telescope · buyable (unlock required)"), tagged when still behind an unlock flag.
   Info-only rows (no locate). **Slice 2 (name the seller) DEFERRED (user, 2026-07-02)** — a live
   `[SHOPDIAG]` dump proved the ERR shop-id layout is customized (single-row shops in 57–58M, gated
   rows in 100201–100338 with ERR-specific flags, no clean Twin Maidens/bell signature), so a vanilla
   shopId-range table would mislabel and the real name needs EMEVD `OpenRegularShop` + talk/**ESD**→NPC
   RE (ESD unparsed) — disproportionate. Detail in the plan's Slice 2 section. **Slice 3 = merchant
   map pins — SHELVED (user, 2026-07-03) after an RE spike.** Confirmed the shop↔NPC join is
   talk-ESD-ONLY (ER EMEVD has no shop instruction — verified via `tools/er-common.emedf.json`; the
   "EMEVD OpenRegularShop" premise was wrong), and that pulling the shop-id RANGE out of ESD needs an
   EzState BYTECODE EVALUATOR on top of the ESD reader (rebuilt as `tools/esd_shop/`) — too much infra
   for one pin category. So the **Merchant ❌ NOT WIRED** map-pin gap (`coverage_vs_mapgenie.md`) stays
   open by choice; Slice 1 search is the shipped merchant feature. Options if ever revisited: (A) full
   ESD+EzState, (C) runtime shop-open hook (visited-only pins). See `plans/merchant_item_search_plan.md`
   Slice 3.
2. **Enemy healthbar NAMES — DONE + in-game validated 2026-07-03 (`feat/enemy-bar-names`).** Mob
   names label the game's own enemy HP bar, mod-agnostic (no bake): tier 1 `NpcParam.nameId → NpcName`
   (invaders/named NPCs), tier 2 TutorialTitle bestiary codex `model*1000 + variant*100 + {10,4}` (ERR
   names every generic mob — sheep/soldiers/trolls; all 3 tiers exercised in-game), tier 3 NpcName
   boss band `9e8 + model*1000 + suffix` (vanilla field bosses, proven via probe). Windows-RE result:
   `docs/re/windows_enemy_name_runtime_source_re_findings.md`. Draw hidden on worldmap/menu; raw
   `screenPos` clamped to the on-screen band (drawn at raw — it's smooth per-frame, an early
   velocity-extrapolation "position-fix" was REMOVED after it caused a jump-ahead on fast camera
   motion). F1 "Enemy bars (mob names)": on/off + offset + text-size sliders. Remaining/future:
   **draw the name directly ON the ER bar widget** (bigger; the current overlay text sits above it).
   Original design notes below. **Key unlock: ERR bundles
   `PostureBarMod.dll` (Mordrog, active in `err_offline.me3`, ImGui+DX12) which already accesses the
   enemy bars — its open source gave the whole recipe.** The engine itself projects each enemy HP bar
   to screen and stores it in the **CSFeMan** HUD manager's per-frame `entityHpBars[8]` array — so NO
   world→screen projection and NO WorldChrMan enumeration needed (the two gaps the earlier RE flagged).
   We read that array, `handle → GetChrInsFromHandle → ChrIns+0x60 npcParam → NpcName FMG` (reusing
   `npc_team_and_name` + `lookup_text_utf8(nameId+700M)`), and draw the name over the bar. Full struct
   map + sigs (CSFeMan `+0x59F0`, EntityHpBar 0x40: handle@0, screenPosX/Y@0x10/0x14, isVisible@0x34;
   GetChrInsFromHandle sig) in **`docs/re/linux_enemy_healthbar_name_re_findings.md`** — offsets are
   live-valid because the bundled PostureBarMod works in-process. Files: `src/goblin_enemy_names.cpp`
   (SEH-guarded host reader, called from the render/present path), `overlay_api::get_enemy_bar_labels`
   (POD getter), draw in `draw_minimap_hud` (independent of the minimap toggle), config `[Enemy Bars]
   enemy_names` (default ON), `[ENEMYBAR]` diag on `debug_logging`. Build+SEH-lint clean, DEPLOYED.
   **NEXT: in-game verify** — relaunch ERR, lock onto / hit a NON-boss mob, the name should appear
   above its HP bar. If wrong/absent: set `debug_logging=true`, grep `[ENEMYBAR] visible=N named=M`
   (visible>0 named=0 ⇒ name-resolve issue; visible=0 with a bar on screen ⇒ CSFeMan sig/offset drift
   — re-pull PostureBarMod at this ER version). Text position (centered above bar) may need an offset
   tweak live. On PASS: add a changelog `Added` line + merge.
   **RUNTIME NAME SOURCE FOR GENERIC MOBS — RE ANSWERED, POSITIVE (Windows, 2026-07-03):** the
   `NpcParam.nameId` path only names bosses/NPCs; the Windows offline RE
   (`docs/re/windows_enemy_name_runtime_source_re_findings.md`) found the missing tiers, all
   mod-agnostic (active install's regulation+msg only): tier 2 = ERR bestiary `TutorialTitle` at
   `9e8 + model*1000 + variant*100 + {10,4}` (names EVERY generic mob on ERR, ~298 models; band
   already routed by `decode_textid`); tier 3 = NpcName band probe `9e8 + model*1000 + 0..999`
   (vanilla/ERR field bosses whose nameId=0 — incl. Tree Sentinel 903251600; these are EMEVD
   `HandleBossHealthBar` nameIds). **Implementation TODO: add tiers 2+3 to
   `src/goblin_enemy_names.cpp`** (strip the `^\d+[a-z]?\.\s*` codex prefix; cache the tier-3
   band scan per model), then the in-game verify above covers both. Risk note: we call GetChrInsFromHandle
   (a game fn) from the present thread (SEH-guarded); if it ever faults, move the read into a
   game-thread hook on `UpdateUIBarStructs` like PostureBarMod does.
   ORIGINAL NOTE — REFRAMED by user (2026-07-02): NOT a new bar, just add mob NAMES.
   The user clarified the real ask: **ER/ERR ALREADY draws the enemy healthbar** (the bar itself is
   done by the game). **Vanilla already shows the NAME for BOSSES on their bar; regular mobs get the
   bar but NO name.** So the feature shrinks to: **display the mob NAME on the existing (non-boss)
   enemy healthbar** — no world→screen projection, no HP-field RE, no drawing a bar. Path: find where
   the game renders the enemy-bar widget + how it fills the boss name, then supply the regular-enemy
   name (NpcParam.nameId → GetMessage, same mod-agnostic resolve already used for enemy-drop labels,
   `docs/memory/features/README.md` Phase 1). Much smaller than the original "draw a 3D bar" scope.
   Still an overlay/RE task, not map-related. (Old scope note kept for reference: entity world_pos is
   available from the NPC-altitude work — `entity_world_pos`/`g_entity_pos` — if a fully custom bar is
   ever wanted instead.)
3. **"Hidden Passage" map category** — MISSING from the MapForGoblins map. Now CONFIRMED tracked:
   regenerated `docs/coverage_vs_mapgenie.md` (2026-07-02, via `tools/coverage_vs_mapgenie.py` +
   the current full-build log) lists **Hidden Passage ❌ NOT WIRED (MapGenie 59)**. RE difficulty
   already documented in `docs/re/windows_group2_landscape_re_findings.md`: hit-detected illusory
   walls, NO action button → no static signal (the HARDEST Group-2 category). The coverage regen
   also dropped NOT-WIRED 31→21 (Elevator + Smithing Table are now correctly wired in the script —
   they were solved since the last regen; the script's SECTIONS/ENUM2DISPLAY were stale).

## Overlay z-order clipping (user report 2026-07-02) — dial DONE, menu-over-map DONE, native clip B3 SOLVED LIVE (2026-07-03)

We render post-present → always on top of the game's own UI. Two sub-bugs:
1. **ERR day/night dial (bottom-right of the map) — FIXED (`fix/f2-fog-locate-v2` branch,
   in-game validated):** static exclusion region (disc ~(1815,1000) r240 + time pill, in the
   1920×1080 virtual canvas, resolution-scaled) wired into `in_draw_bounds` so markers +
   hover + pile anchors cull together. ERR-gated, ini `clip_game_ui` (default true).
2. **Menus opening OVER the map (fast-travel confirm, marker dialog, etc.) — SOLVED + WIRED
   (2026-07-03), pending one live re-check.** Flag = **`CSMenuMan+0x104` (u8)**: 1 while a
   submenu covers the open map, 0 on the bare map (incl. pan/zoom). Found by live in-DLL
   byte-diff on Windows (the RE loop DOES run here, not just Linux). Full write-up:
   `docs/re/worldmap_menu_and_native_clip_re_findings.md`.
   - **Wired:** `goblin::worldmap_probe::menu_covers_map()` (reads +0x104, `GOBLIN_RENDER_API`),
     `render_markers` early-outs the whole worldmap pass when `clip_game_ui && menu_covers_map()`,
     RPC `status` gained `menucover=`. Discovery scaffold kept: `dump_menu_state` dumps CSMenuMan
     +0x0..0x400; `menu_open_diag` (ini `[Debug] debug_menu_cover_diag`) is now **byte-wise** —
     the first int32-stride/`[0,256)`-filter version silently dropped byte flags (the bug behind
     two "zero logs" runs; see findings).
   - **VALIDATED live (2026-07-03):** `menucover=` flips for multiple covering menus and markers
     visibly disappear under them. **Task A complete.**
3. **Native map CLIP rect (perfect edge clipping) — Task B, B3 SOLVED LIVE (2026-07-03).** Full
   detail: `docs/re/worldmap_native_clip_b3_scaleform_re_findings.md`. Progress:
   - **B1 (live struct rect-dump) — done, negative.** No screen-space map-viewport rect is parked
     on the canvas / dialog / view / viewmodel; only the full 1920×1080 canvas + marker-space rects.
     Also learned: the in-game res slider does NOT change the DXGI backbuffer (ER draws the map into a
     fixed 1920×1080 virtual canvas and up/downscales) → the clip is in canvas units.
   - **B2 (D3D12 scissor/viewport command-list hook) — dead end.** ER ships the Agility SDK
     (`D3D12Core.dll`); MinHook AND a direct vtable-swap on `RSSetScissorRects`/`RSSetViewports` both
     **never fire** (not even viewports, every frame) — the engine's render calls don't read the
     vtable slot we can reach. Scaffolding kept OFF under `debug_scissor_probe`; don't re-attempt.
   - **B3 — SOLVED via LIVE RPM (not Ghidra).** Instead of the static-RE the prompt budgeted, an
     external ReadProcessMemory scan (scratchpad `GfxScan.cs`: RTTI→heap→movie-name match) found the
     live Scaleform viewport in one session. Chain (RTTI-verified, probe-reachable):
     `WorldMapDialog + 0x140 → movieHandle + 0x00 → MovieImpl + 0xB0 = int L,T,W,H` (clip rect,
     **canvas 1920×1080 units**; buffer size at `+0xA8`; `movieHandle+0x58` = the worldmap
     `CSScaleformSwfPlayer`). **Answers the user's question decisively: live RPM was far more
     efficient than offline Ghidra here.**
   - **IMPORTANT correction to the plan's premise:** the movie viewport is the **FULL canvas
     (0,0,1920,1080)** — no engine inset sub-rect (the dial is HUD-over-map, inside the viewport).
     So the native clip retires the **edge/void cull (#1)** but does **NOT** remove the dial disc
     (#2) or user rects (#3) — keep `in_game_ui_exclusion` + `ui_exclusion_rects` as a separate
     HUD-overlap layer. One axis unverified (needs a non-16:9 **window** resize): whether `+0xB0`
     goes letterbox-inset off 16:9. Consumer wiring in the findings doc; keep the `clip_game_ui` gate.

## Silent deadlock freeze + freeze watchdog (2026-07-02, `fix/f2-fog-locate-v2` branch)

User hit a "deadlock-like" FREEZE (18:49): last log = a normal `render.minimap` BENCH line,
then silence — NO exception, NO crash dump, window solid, DLL threads (RPC listener) alive.
Distinct from the `eldenring.exe +0x1EB9999` exit crash (that one is ER's own deterministic
teardown crash — 6/6 identical stacks across the day, fires on Exit/Alt+F4; our handler now
calls TerminateProcess after the triage so it closes the game instead of leaving a Wine
zombie, which was ANOTHER freeze-looking failure mode). The real deadlock is UNSOLVED — no
stack yet. Shipped the tool to catch it: **freeze watchdog** (`goblin_freeze_watchdog.cpp`,
ini `[Debug] freeze_watchdog_secs`, default 20s, 0=off) — present-thread heartbeat; on a
stall it writes `logs/MapForGoblins_freeze_<pid>.txt` + a FULL all-thread minidump from the
healthy watchdog thread; raw Win32 on the dump path (spdlog could be the deadlock). **Next
freeze → symbolize the dump's MapForGoblins frames with the deployed PDB and root-cause.**
Also mandatory now: `docs/memory/tooling/mfg-rpc-driver-hardening.md` (RPC `ping` ≠ game
alive; driver scripts need per-call timeouts + liveness gates — learned when a freeze left a
validation script spinning forever).

## Overlay i18n v1 — SHIPPED + in-game validated FR (2026-07-02, `feat/overlay-i18n-v1`)

`goblin::i18n::tr()` (host module, GOBLIN_RENDER_API-exported) + ini `overlay_language`
(auto|en|fr|…) + disk table `lang/<code>.txt` (en=/tr= pairs, \n-escaped; missing → EN
fallback). ~180 tr() sites across the panel section files + map_renderer tooltip glue;
category/section labels translated at display (incl. the alphabetical sort + the category
search box matching EN or FR); `Filter::match` also matches translated per-block keyword
strings so the settings search works in both languages ("echelle" → Marker scale, validated
in-game). `assets/lang/fr.txt` ships ~270 translated strings; build.bat packaging copies
`assets/lang/*.txt` into the package (ERR: `dll/offline/lang/`; profiles: `MapForGoblins/lang/`)
— **packaging path NOT yet exercised on the Windows box** (next `build.bat snapshot` run
verifies it). Gotchas: (1) `auto` reads the WINE prefix locale under Proton (usually en_US
even on a French desktop) → French users set `overlay_language = fr` explicitly — the
deployed dev ini has it; (2) avoid `œ` in translations (outside the merged font ranges —
use "oe"); (3) translated format strings must keep the % placeholders in order; (4) keep
label translations ≲ English+20% — the panel auto-fits but caps at 840px (was 720; the
first FR pass clipped at the old cap, user-reported → cap bumped + longest FR labels
shortened, both in-game verified). Deliberately
NOT translated (v2 candidates): quest-browser step CONTENT (hand-authored corpus, big),
dev-only sections (P2b/sprites/grace-debug/dev-tools), spdlog lines. The same lang-file
mechanism can carry future languages — drop a `lang/de.txt` etc., no rebuild.
**Live switch added same day (`feat/i18n-live-switch`, in-game validated fr→en→fr):** F1
"Language:" combo (under the master row; entries = auto, en, + one per `lang/*.txt` scanned
while the combo is open) calls `i18n::set_language()` — same-frame table swap, no restart.
Safe WITHOUT locking because every tr()/set_language caller is on the PRESENT thread (panel,
renderer, RPC pump — documented in goblin_i18n.hpp; keep it that way). `generation()` bumps
per load; the category sort order re-sorts on it (order follows the displayed labels). RPC
`set overlay_language <code>` also swaps live (wired in config_set_by_key — how the
validation drove it). Persist = the normal "Save to INI".

## Clang-only Phase 1 — WINDOWS BUILD + SNAPSHOT VALIDATED (2026-07-02) → only the in-game matrix left

`build.bat` is now ninja+clang-cl (no VS/msbuild; tool paths env-overridable, defaults = the
Windows box per `build-toolchain-clang-xwin.md`); `/Brepro` determinism PROVEN on Linux (relink →
identical md5); PDB pairs archived to `pdb-archive/<ver>-<profile>/`; `tools/lint_seh.py` guards
the SEH-elision regression. Old `build/` msbuild dir is disposable (`build-err/` replaces it).

**Default build VALIDATED on Windows (2026-07-02):** `build.bat` (ERR) ran clean end to end —
auto-configure (CMake 4.1 + Ninja + `clang-cl-xwin.cmake`, Clang 22.1.8), `[80/80]` compile+link →
`[SUCCESS]`, `build-err/{MapForGoblins.dll 4.6 MB,.lib,.pdb}`. Points to note:
- **0 real errors.** The only `Failed` line is `Performing Test CMAKE_HAVE_LIBC_PTHREAD - Failed` —
  expected on Windows (no pthread), CMake falls back correctly.
- **340 warnings, all benign / third-party**, two dominant recurring sources: (1)
  `-Wdeprecated-literal-operator` on `operator"" _a` inside vendored spdlog bundled-fmt; (2)
  `-Wdeprecated-declarations` on `std::wstring_convert`/`<codecvt>` in `src/from/params.hpp:17`
  (deprecated C++17, still functional). Both suppressible if we ever want a clean log
  (`_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING`); not worth churn now. (`snapshot` adds imgui
  `-Wnontrivial-memcall` on its `memset(this,…)` ctors — same benign class.)

**`build.bat snapshot` VALIDATED on Windows (2026-07-02) — packaging + PDB archive proven.** Full
`pre-1.0.18` (ERR) snapshot ran end to end: data pipeline (964 MSB, 28313 placements, 0 MSB errors)
→ reconfigure `-DVERSION_PRE=pre` → `[64/64]` link `[SUCCESS]` → `mfg_inigen` INI → package under
`pre-release/` (`dll/offline/{dll,ini}` + `addons/MapForGoblins/menu/02_120_worldmap.gfx` + LICENSE)
→ **PDB pair archived to `pdb-archive/pre-1.0.18-err/` (DLL + 27 MB PDB, NOT shipped in the package)**
→ README version-substituted (`vpre-1.0.18`). Crash-symbolication chain verified: shipped DLL is
**byte-identical (SHA-256)** across `pre-release/`, `pdb-archive/`, `build-err/`.

**Still open:** `build.bat release` (version-bump path) un-exercised; then Phase 2's real in-game
validation matrix. Docs already flipped to "clang = canonical" + `steam_api64.lib` removed. See
`docs/plans/clang_only_toolchain_plan.md` Phase 1/2.

## Two new plans scoped (2026-07-01): big-files refactor + clang-only toolchain

`docs/plans/big_files_refactor_plan.md` (god functions/duplication across the 7 biggest hand-written
files; **items 1+2 DONE 2026-07-02** — item 1: draw_panel split into `src/overlay_panel/` 8-file
section layout, render cpp 2150→~400 lines, in-game validated; item 2: shared marker gates, see the
DX-sweep entry below — remaining: 3 classify dedup, 4 diag quarantine, 5 icon_uv, 6 god functions,
7 grace-sprite design) and
`docs/plans/clang_only_toolchain_plan.md` (retire MSVC; USER DECISION reverses the same-day "MSVC
canonical" note in `docs/memory/tooling/build-toolchain-clang-xwin.md`). **Phase 0 update: the 3
`__try`-elision hazards (world_position per-frame probes + tutorial_popup init poll) are FIXED
(`5b80541`, built + deployed); still open = repo-wide `__try` classify pass, then Phases 1–2.**

## Feedback round 3 (2026-07-02, `feat/per-marker-native-glyphs`) — church glyph, fragment leaks, pin-RE diag

- **Churches drew circle:** the worldmap gfx has NO numeric `MENU_MAP_03` — the church glyph is
  NAME-keyed (`MENU_MAP_Church`). Fix: `map_point_name_alias()` in MapPointProvider (numeric
  resident → numeric disk → alias resident-by-name → alias disk-by-name). Checked the decompiled
  gfx symbol list: also missing numerics with NO known name = **15 (wells), 43 (Gelmir camp)** →
  those stay circle until a name/RE surfaces.
- **require_map_fragments leaks**, 3 root causes fixed:
  (1) QuestNpcLayer never set `fragment_flag` → quest pins ignored the gate; now derived from the
  pin's overworld tile (`quest_pin_fragment_flag`; UG groups stay ungated — tile ambiguous).
  (2) Off-shore islet tiles (Divine Towers) missed the ±1 neighbour fill → widened to ring ±2
  (`GetMapFlagFromTile`).
  (3) Unmappable AREAS (Roundtable m11_10, unconverted dungeon leftovers) returned 0 = "always
  shown" → `marker_fragment_flag` now returns a never-set sentinel flag (INT32_MAX) for a
  projected area outside {60,61,12,40-43}, so the existing renderer gate hides them while the
  toggle is on. Rune-piece leak most likely class (2)/(3) — recheck in-game.
- **Native pins still visible — RE diag added:** debug_logging now logs `[LANDMARKPIN] WMPP
  iconIds NOT ours (never suppressed): 41x191 67x26 80x…` at build. Expected residents: 41/67
  (bosses), 80 (graces), 83/84/85 (structural), 42 (sub-zones), 87 (VM requests), 0 (ERR arena).
  Anything the user still SEES beyond those → match its iconId here, then decide suppress/classify.
  NB: ERR also injects its OWN custom rows (stakes/pools/effigies at iconId 374+ etc.) — those are
  OUR old native-injection families, not WMPP vanilla pins.
- **Round 3 in-game results (user, 2026-07-02):** church glyph FIXED; fragment leaks mostly fixed;
  Roundtable items still visible — ACCEPTED as correct (its markers project through the m11 conv
  onto a Leyndell-fragment tile the player owns; the "always available since game start" reading
  fits the Roundtable anyway). `[LANDMARKPIN]` diag: `19x3 41x191 42x29 67x26 80x73 83x70 84x16
  85x16 87x8` — everything expected EXCEPT **iconId 19 (Windmill Pastures, 3 rows): missed in the
  first parity grouping pass** → added to `WorldTownVillage` (those were the user's remaining
  mystery pins).
- **Crowded-icon DX: hover FAN-OUT (spiderfy) — DONE + IN-GAME VALIDATED 2026-07-02
  (`feat/spiderfy`, entirely via the Phase 4 RPC loop — boot→save→map→hover all scripted, both
  states screenshot-verified, debugged live with a hot-reloaded debug-viz build).** Hovering a
  pile fans its members out (ring ≤12, archimedean spiral above, cap 40 + "+N more"), legibility
  backdrop disc, leg lines, per-member hover tooltips via the existing hover_test; sticky on the
  pile's tile cell key, closes when the cursor leaves fan extent + margin. Ini `cluster_spiderfy`
  (default on). Loop side-findings fixed on the way: RPC `mouse_move` needed the SetCursorPos
  TRAMPOLINE (SendInput absolute lands off-target under Wine) + a ±1px relative jiggle (a real
  mouse EVENT — otherwise the game re-warps the OS cursor onto its raw-input reticle and the
  placement lasts one frame); drivers should send mouse_move twice (rare warp race eats the
  first).

  **Spiderfy v2 (same day, user followups, both live-validated):** (1) zoomed-out GATE — no fan
  when one 256-unit map tile projects below 64 SCREEN px (zoom-range-agnostic criterion); pile
  tooltip still works there. (2) member DEDUP — fan shows ONE icon per distinct
  (name_id, category, icon) with an xN badge + "(xN in this pile)" tooltip line; a 52-pile
  rendered ~20 fanned entries with x2/x3/x5 badges instead of a 40-icon spiral.

## Phase 4 session findings (2026-07-02, spiderfy-v2/RPC-HUD run) — DX sweep + background-focus blocker

- **RPC command HUD shipped + live-validated:** bottom-right overlay feed of driver commands +
  results (host-drawn, `debug_rpc::recent_commands`, entries fade after ~6s; empty = draws
  nothing). New RPC input commands: `mouse_wheel <±notches>` (map zoom — validated), `type
  <text>` (focused ImGui field). **Wine/AZERTY key gotcha #2:** SendInput VK→scancode uses the
  US layout but the return scan→VK translation uses the HOST layout — so VK_A lands as 'q' on
  AZERTY. `type` can't compensate in-process (the game-side layout reads as US; VkKeyScanW is
  identity) — the DRIVER must send the QWERTY-position character (send "lqrvql" to type
  "larval"; a↔q, z↔w, m→';'). Proven live: search field filled "larval", 3 matches listed,
  result click panned the map + drew the search ring.
- **THE systemic background blocker found (answers the user's "stuck when unfocused" question
  for real): it is NOT the pause** (`paused=0` throughout) — **our own `g_has_focus` gate kills
  the keyboard poll AND mouse clicks when the game window loses focus**, so RPC UI driving
  (typing, clicking results) dies as soon as the user works elsewhere. SetForegroundWindow from
  in-process does not reliably steal X focus back under Wine. Next work: a dev-mode override
  (treat-as-focused while debug RPC is enabled — accept the "user keystrokes leak into the
  background game" tradeoff, it's symmetric with PauseTheGame's global keys) or real X-side
  focus forcing. Until then: leave the game window focused during scripted UI runs (world/map
  driving via `key`/menus works regardless — game reads its own input path).
  **Partially closed 2026-07-02 (`fix/f2-fog-locate-v2` branch, user-reported + repro'd +
  validated live): the FIRST `key` command after an auto-refocus was silently lost** (async
  X/Wine focus; sometimes with no WM_KILLFOCUS at all). `ensure_game_foreground` now waits for
  foreground + our g_has_focus gate, and `key` is CLOSED-LOOP: a keyboard-arrival counter in
  hk_wndproc (WM_KEYDOWN leg + RIM_TYPEKEYBOARD raw leg — gameplay is NOLEGACY, legacy leg
  alone false-retries everything) is polled post-send; no arrival ≤240ms → refocus + resend
  once (down,down,up = one logical press → can't double-toggle). `status` gained `kbseen=`/
  `fg=`. NOT yet covered: `mouse_click`/`type` have no delivery verify (same loss window),
  and the full "drive UI while user works elsewhere" case still needs the dev-mode
  treat-as-focused override.
- **Great Rune "(x2)" in search — NOT a bug (triaged with the user, 2026-07-02).** Every rune has
  TWO legit markers sharing the GoodsName: the boss-drop (live boss position — Mohg's is on the
  UNDERGROUND page, the tell that unmasked it) and the ACTIVATION site (Divine Tower — ring
  visually confirmed on the Divine Tower of Limgrave). Possible polish later: suffix the
  activation-site marker's tooltip ("(activation)") so search rows self-explain.
- **Endless map pan after F1 close at screen edge — FIXED + in-game validated (2026-07-02,
  `fix/f1-close-edge-pan`, fully via the RPC loop: scripted boot→save→map→edge→close, pan
  stop proven by screenshot diffs).** On the menu-close falling edge (map open, real close),
  the cursor is nudged out of ER's edge-pan band via `set_cursor_pos_real` + the ±1px SendInput
  jiggle (the game only adopts the position on a REAL mouse event — same as RPC mouse_move).
  Non-obvious finding: **the edge-pan band is ~150px wide at 1080p with speed falloff** — the
  first 64px nudge landed visibly on-screen yet the map kept panning; margin is now
  `max(64, height/6)`. Full detail `docs/memory/bugs/f1-close-edge-pan.md`. (No changelog
  entry — F1 is fork-added, intra-cycle defect.)
- **Quest-NPC altitude badges missing — FIXED (`fix/npc-altitude-badge`,** user question): the
  `entity_world_pos`/`g_entity_pos` path dropped MSB Y, so NPC pins never got the ▲/▼ badge.
  Y now threaded (EntityPos.wy from DiskEnemy/DiskCollectible.posY). In-game visual confirm
  pending (needs an NPC pin at a different altitude than the player).
- **Boot-time work — steps 1+2 DONE, in-game measured (2026-07-02,
  `perf/boot-io-profile-param-poll`).** (2) **param_poll fixed:** the 5005ms was NOT the poll
  interval (already 200ms) — it was the "honor `load_delay` as a MINIMUM total wait" sleep after
  the poll had already confirmed readiness. Ready now proceeds after a 250ms settle; `load_delay`
  kept only as the fallback minimum when the poll can't confirm (ini comment updated). Measured:
  `init.param_poll` **5005 ms → 250 ms**, "Initialization complete" **t+10.7s (was ~15s)**, SIG
  30/30, zero errors. (1) **Boot I/O profile shipped + run:** new `diag_boot_io` ([Debug], default
  off) widens the CreateFileW observer to ALL opens and arms it LIVE at the top of setup_mod (new
  `modutils::hook_now` — the normal queued path only goes live at enable_hooks, ~14s in, which is
  why the old observer couldn't profile boot). `[BOOTIO]` = +ms-since-arm, per-open latency,
  ok/FAIL, first 1500 lines then counted. **Findings (ERR/Proton run 2026-07-02): boot is NOT
  file-I/O bound** — 1500 opens totaled ~85ms of open latency, all sub-ms. Time lives in idle
  gaps between opens: t+0-7s engine init (sparse opens; a 1.5s gap around failing discord-ipc
  pipes, ~1.3s after the sd/cs_smain.bnk audio bank), regulation.bin opened t+7.1s then a 1.35s
  gap (decrypt/parse; from_params completes t+9.3s), then OUR MSB disk-loot burst (~964 MapStudio
  opens, t+10.4-11.2s ≈ 0.8s, matches the known prebuild cost). FAIL noise is benign (158×
  mods' log.txt probes, dxvk.conf, discord-ipc absent, PauseTheGame's pause_keybinds.ini).
  **Step 3 (only if wanted): targeted wait-hooks are the remaining tier — the data says candidates
  are the discord-ipc connect wait and the audio-bank gap, NOT file I/O; the regulation parse is
  engine work (dangerous tier, leave alone).** vkd3d shader-cache hygiene + SkipTheIntro remain
  the safe game-side levers.
- **DX sweep findings (multi-zoom):** (1) markers OUTSIDE the map canvas — **FIXED 2026-07-02
  (`refactor/marker-gates-clip-labels`, in-game validated via the RPC loop):** the engine's static
  full-map rect (view+0x350, now in `LiveView.mapMin/Max*`) is projected through the marker view →
  `PushClipRect` around the whole worldmap pass + the cull/pile-visibility tests use it (hover dies
  with the pixels). Validated on the morgott locate-clamp frame: the black void past the canvas
  edge went from dozens of floating icons to ZERO. NB the earlier "markers all over black at max
  zoom-out" screenshot was NOT this bug — that's undiscovered-canvas INSIDE the map rect with
  `require_map_fragments=false` (the deployed dev ini) — flipping it true live collapsed the spam
  to the discovered region (also a live proof of the new shared gate predicate). (2) pile label
  flood at far zoom — **FIXED same branch:** labels now gate on the fan's tile-px criterion
  (256-unit tile < 64 screen px → hidden; pile tooltip still names the location). Same session
  landed **big-files refactor item 2**: `marker_passes_gates()` (the 4 event-flag gates, worldmap +
  minimap now ONE predicate) + `refresh_player_world_y()`; icon-scale math deliberately NOT shared
  (minimap's base+clamp differs by design, see item-13 note in draw_minimap). Still open: (3) fan
  near screen edge overflows off-screen (no clamp/re-anchor — the canvas clip now trims it but
  doesn't re-anchor); (4) ER edge-pans when the cursor sits near a screen edge — scripted
  mouse_move near edges drifts the view between screenshots (driver beware, not a bug).
- **F2 (fog locate clamp): FULLY REPRODUCED, deterministic scripted recipe (2026-07-02).**
  With the map open + F1: type `godrick` (search works via RPC — no AZERTY remap needed for that
  word) → clicking "Godrick the Grafted (x1) - Overworld" pans fine (Stormveil is DISCOVERED on
  this save, ring visible = negative control). Then `morgott` → click "Morgott, the Omen King
  (x1) - Overworld" (Leyndell, deep fog) → **the pan CLAMPS at the edge of the pannable/revealed
  area: the map canvas ends, the rest of the screen is void, and the target is never centred (no
  search ring in view)** — exactly F2's description. Repro screenshot kept at
  `/tmp/mfg_rpc_test/f2_repro_morgott_clamp.bmp` (regenerate any time with the recipe above).
  Bonus: that same frame is the strongest capture of the "markers drawn OUTSIDE the map canvas"
  sweep finding — dozens of icons float on the black void past the map edge, over the day/night
  dial. Fix direction per the original F2 note: pan-OOB support (bypass/extend the panX/panZ
  clamp in the projection when centring on a locate target).
  **FIX ATTEMPT 2026-07-02 — REVERTED (user call), read
  `docs/re/linux_f2_fog_locate_clamp_re_findings.md` BEFORE retrying.** Net RE knowledge kept:
  the real blocker is the engine clamping the cursor RETICLE to discovered-extent bounds INSIDE
  the c32f0 step (bounds source absent from the view/cursor structs — needs Ghidra on the c32f0
  subtree); direct pan writes and snap-rect (+0x340) widening are PROVEN dead ends (composite/
  overlay divergence; off-map teleport); `cursor+0x180` = the zoom easer TARGET (writing it makes
  the engine zoom itself — mechanically works, rejected as UX: uninvited zoom on every locate +
  visible 90-frame flicker fight on clamped targets). Constraints for any retry (user, hard):
  non-fog locates must behave EXACTLY as today, no per-frame write fights, no forced zoom.

## SINGLE-DLL migration — profiles retired (2026-07-02, `feat/mapgenie-landmark-parity`)

User audit request confirmed profiles had ~no reason left: zero real per-profile DATA (the only
divergent bakes were EMPTY non-ERR stubs of ERR-only tables + `legacy_conv`, which is a pre-param-
residence fallback only — the live `goblin_legacy_fold` is primary). Implemented:
- `goblin::err_features_enabled()` (goblin_config.cpp) replaces compile-time `profile_is_vanilla()`:
  **ERR = `reforged.dll` loaded in-process** (GetModuleHandle at init; cached; logs `[PROFILE]`).
  Every ERR me3 profile lists reforged.dll BEFORE MapForGoblins so it's resident when our DllMain
  runs. v1 was a disk fingerprint (`menu/deploy/projects/ELDENRINGReforged`, in-game verified
  DETECTED on ERR) but disk presence false-positives when the SAME install hosts a vanilla.me3
  launch (ERR files on disk, not loaded) — the loaded-module check answers "what's RUNNING".
  ERR-only config force-disabled off-ERR at load, exactly like the old vanilla build.
- DELETED: `GENERATED_SUBDIR` CMake machinery, `MFG_VANILLA`/`MFG_PROFILE_VANILLA` defines,
  `tools/gen_nonerr_stubs.py`, `src/generated_{vanilla,erte,convergence}/` dirs (were gitignored),
  per-profile build dirs. `src/generated/` is THE single bake dir.
- build.bat `--vanilla/--convergence/--erte` KEPT but now only select packaging assets
  (README/gfx/SNAP_DIR) + offline pipeline data source; all profiles build/ship the same DLL from
  `build-err/`. inigen always emits the full ini (ERR entries included everywhere).
- `liveLootLabels` single default = false (vanilla package used to default true — changelog notes it).
- **BOTH SIDES VERIFIED in-game (2026-07-02), same install, same DLL:** ERR launch →
  `[PROFILE] ERR DETECTED (reforged.dll loaded in-process) — ERR-only config active`; vanilla me3
  launch (`internals/modengine/bin/me3 launch -g eldenring -e "<steam exe>" -p vanilla.me3`, runs
  fine from Linux) → `[PROFILE] ERR not detected — ERR-only config force-disabled`, 0 errors.
  Bonus mod-agnostic proof: the parity landmarks read the ACTIVE regulation — `[LANDMARKLIVE] 280`
  on vanilla vs 295 on ERR with coherent per-category diffs (Evergaols 10 vs 16 = ERR's added
  archery-challenge rows, Dungeons 63 vs 66, …). Still open: Windows `build.bat` + `build.bat
  snapshot` re-run (script edited; the validated snapshot flow predates this change). Detection
  risk: if a future ERR renames `reforged.dll`, add its new core native to the check.
- Baked-data plan impact: Phase A per-profile regen is MOOT; remaining bake work (name_regions/
  region_anchors → disk-MSB runtime, icon atlas) unchanged.

## Native-pin PARITY — 9 new landmark categories IMPLEMENTED + in-game VERIFIED (2026-07-02)

`feat/mapgenie-landmark-parity`: full audit of native WMPP pins (every family the game still draws
that we didn't re-draw) → 9 new categories via the same `build_live_landmarks` pass: Churches /
Ruins / Rises & Towers / Shacks / Forts / Castles / Towns & Villages / Colosseums / Unique Sites
(~167 rows; + iconId 62 Ashen Leyndell → LegacyDungeon). All WMPP pins are natively eventFlag-gated
(discovered-only) — we show all. Full audit table + skip rationale (42 sub-zones, 87 quest markers,
0 ERR-arena) in `docs/memory/features/mapgenie-landmark-categories.md`. ERR build DEPLOYED to
`~/Games/ERRv2.2.9.6/dll/offline/`. **Next: in-game verify** — grep `[LANDMARKLIVE]` (now a loop
log listing all 16 landmark cats; expect Churches ~28, Ruins ~38, RisesTowers ~21, Shacks ~24,
Forts 7, Castles 6, TownsVillages ~14, Colosseums 3, UniqueSites ~26), toggle a few in F1, then
merge. Colosseum got a `category_gpu_iconId` (24) native-glyph entry, rest = circle.
**VERIFIED in-game on ERR (2026-07-02): `[LANDMARKLIVE] built 295` — all 16 categories, every
count exactly as predicted; SIG 29/29 clean, 0 errors.** Ready to merge.

**Side finding (SUPERSEDED same day by the single-DLL migration above):** the non-ERR profile
bakes were stale/incomplete (vanilla missed the whole Group1/2 enum block → its build had been
broken since Group 1; erte/convergence missed whole files). Fixed first by syncing headers, then
made moot by deleting the per-profile dirs entirely.

## Native landmark-pin suppression — IMPLEMENTED, not yet in-game verified (2026-07-02)

`feat/native-landmark-pin-suppression`. **Path decision (user asked grace-path vs direct):** direct
areaNo=99 flip on the native WMPP rows — the grace SetTo draw-only hook exists ONLY to preserve the
fast-travel click, and landmark pins have no click action (tooltip-only, duplicated by our overlay);
the direct path needs zero new RE and reuses the proven eviction/restore pattern. Implementation:
registry in `goblin_section_visibility.cpp` (`reset/register/apply_native_landmark_suppression`,
`goblin_inject.hpp` facade, GOBLIN_RENDER_API-annotated for the hotreload split);
`build_live_landmarks` owns the lifecycle (reset RESTORES before clearing so a rebuild never bakes
a flipped 99 into markers/orig_area; the param iterator yields live-row references so `&row` is the
live table); the `menu_auto_toggle_loop` watcher re-applies on landmark-category / F10 master
flips (`take_native_landmark_dirty`); config `landmark_suppress_native` (World section, default
true, NOT ERR-only — vanilla native pins duplicate too). Per-category: only categories toggled ON
suppress their native rows; graces (80) / bosses (41/67) untouched. Flips take effect next map
open (same cadence as every areaNo owner). **CONFIRMED in-game (user, 2026-07-02)** with one gap:
minor-dungeon families (Caves / Hero's Graves) kept their native pins — their OVERWORLD pin comes
from the dist-view mark, so `areaNo_forDistViewMark` now flips to 99 alongside `areaNo` (both
saved/restored). **ALL CONFIRMED in-game (user, 2026-07-02): dungeon pins suppressed too.**

Same feedback round shipped 3 more fixes + a round 2 (deployed `e970671e`) — **ALL CONFIRMED in-game (user, 2026-07-02)**:
- **Collected black-disc bug:** the icon-legibility contrast disc under small icons kept full alpha
  when the icon dimmed as collected → looked uncollected. Disc alpha now follows the icon tint's
  alpha (`draw_legible_icon`, map_renderer.cpp).
- **"Map icons: ON/OFF" toast removed:** its only remaining trigger is the F1 menu's own master
  checkbox — announcing what the user just clicked was noise. `show_toggle_banner` kept (unused)
  for a future hotkey path.
- **Stakes/Pools glyph collision (v2 after user feedback on v1's swap):** both categories now draw
  the SAME native Marika-statue glyph (`MENU_MAP_89` — matches both in-world objects) differentiated
  by TINT (pools = multiplayer blue, stakes = warm gold — the game's own colour language).
  `MENU_MAP_21` reads as a lift platform (it IS the Grand Lift WMPP glyph — landmark GrandLift
  already used it) → given to `WorldElevator`, grey-tinted. Plumbing: `CATEGORY_GPU_ICONS` grew
  per-category `scale` (these 3 POI glyphs draw at 0.6× the 2.2 mapSymbolScale — at raw scale they
  dwarfed item dots, user-reported) and `tint` (ABGR multiplied into the draw tint via `mul_tint`,
  composes with collected-dim/boss-red). SB_MapCursor sheets re-checked with menu_tex_extract: no
  dedicated stake glyph exists natively; 21/89 rects eye-confirmed.
- **Spoiler-free coverage audit (user ask):** the only ITEM-identity leak outside `lot_backed` was
  Farmable Drops (non-lot, names the real notable drop) → `anonymous_marker(m)` = lot_backed ∨
  farmable now gates the "?" draw + tooltip. Deliberately NOT anonymized: pieces/kindling (identity
  IS the category), material nodes (fixed gather spots, randomizers don't touch them), world
  features. Enemy/EMEVD drops were already lot-backed → covered.

## Native-map landmark icon suppression — TRACKED (2026-07-02) → implemented same day, see above

The game still draws its OWN landmark pins on the native world map (Minor Erdtrees etc.), so our
new landmark categories (incl. the 5 that now use the native glyphs — DivineTower/Evergaol/
MinorErdtree/GrandLift/MiquellaCross) can visually DUPLICATE the native pins when toggled ON.
User decision: acceptable for now; the definitive fix is suppressing the native pins. Technical
precedent exists: native grace suppression (`goblin_grace_suppression.cpp`) and the areaNo=99
row-flip eviction used by section visibility — the same trick applied to the landmark
WorldMapPointParam rows (gated on "our category is ON", restore on OFF) should kill the native
pin without touching files. Scope when picked up: which iconIds to suppress = exactly the ones our
categories re-draw; keep ERR's own custom pins (boss/camp) untouched. **NB (2026-07-02): do this
AFTER the parity branch above merges — the suppression set is now the Group 1 + parity iconId
union (see the audit table in `docs/memory/features/mapgenie-landmark-categories.md`).**

## Group-2 Elevator MECHANISM SOLVED on Linux (2026-07-02) — implementation open

Full RE chain in `docs/re/linux_group2_prompt_binding_re_findings.md` (done entirely with in-DLL
probes + offline python — first end-to-end run of the Linux RE path). Net: recon's 5010 anchor was
LADDERS; real lever-lifts = ABP 8200-8501 "Pull/Push lever" -> ~55 ObjActParam rows (join col
+0x28) -> **MSB ObjAct EVENT section binds {asset entity, objActParamId}** => Elevator category is
a pure mod-agnostic disk parse (msbe: parse ObjAct events, filter param ids whose ABP text is a
lever, join asset position; refine by anim-id groups if gates over-capture). Smithing Table
SOLVED too (2026-07-02): [ASSETRADAR]+[ASSETCOUNT] -> model filter **AEG099_308**.
**BOTH IMPLEMENTED + MERGED to master + in-game verified (user, 2026-07-02)**: 54 Elevator markers
(AEG027_* lift family, positions near-perfect), 4 Smithing Tables. Branch
feat/mapgenie-group2-elevator-smithing merged; worktree cleaned. Group 2 remaining per the recon
doc: whatever the next MapGenie diff lists (see coverage_vs_mapgenie.py). Probes live in `src/goblin_param_scan.cpp`
([PARAMSCAN]/[EMEVDSCAN]/[ABPTEXT], debug_logging-gated).

## Linux runtime-RE path — investigate to stop the two-PC switch (2026-07-01, not started)

User pain: runtime RE is Windows-by-convention but the live game runs on the Linux box (Proton).
Options + trial plan in `docs/memory/tooling/linux-runtime-re-options.md` (default = in-DLL probes,
first trial = ceserver + CE GUI on the Proton pid). Related vision note (runtime modding framework,
NOT a plan): `docs/runtime_modding_framework_vision.md` — **capabilities-vs-vision GAP AUDIT done
2026-07-02: `docs/runtime_live_capabilities_audit.md`** (what's proven live, what's missing for a
full no-regulation.bin runtime mod, recommended battle order: FMG-inject + param_set_field quick
wins → param_add_rows pivot → items/events → file-resolution hook long-term).

## MapGenie category coverage — GROUP 1 MERGED; GROUP 2 (Portal) RE in progress (2026-07-01)

**GROUP 1 landmarks — MERGED to master + ERR in-game confirmed.** 6 `World -
DivineTowers/Evergaols/MinorErdtrees/GrandLifts/Dungeons/LegacyDungeons` built LIVE from
`WorldMapPointParam.iconId` (`build_live_landmarks()`, `src/worldmap/map_entry_layer.cpp`). `[LANDMARKLIVE]
built 114` (counts match off-disk), positions correct. Ghost = existing `WorldHostileNPC` (no work). All
default OFF. Circle-glyph followup outstanding (task chip spawned; see memory note).

**Miquella's Cross — added (branch `feat/mapgenie-group2-portal`).** DLC iconId 208 (13 rows) reuses the
same landmark pass — one enum entry + iconId branch. Built clean, deployed to `dll/offline/`. Restart ERR
+ grep `[LANDMARKLIVE]` (should now show `MiquellaCross 13`). Default OFF.

**Miquella's Cross + GROUP 1 landmarks — MERGED to master** (`5febe5f`).

**Portal (Group 2) — IMPLEMENTED on branch `feat/mapgenie-portal`, build-clean + deployed, NOT yet
in-game verified.** RE fully solved (`docs/re/windows_portal_aeg_re_findings.md`): a portal = an
**`AEG099_510`** sending-gate asset whose EntityID is bound as **arg[2] of EMEVD warp template
`90005605`** (the mod-agnostic "actually warps" signal; isolates ~23 real gates from the model's ~180
placements). Runtime pass: `msbe::parse_emevd_portal_gates` harvests the gate entity set from
`event/*.emevd` (shared with `load_emevd_world_feature_flags`), `build_disk_portal_markers` emits each
`AEG099_510` disk asset (aegRow 99510) in that set, dedup by entity. Label = PlaceName 6108700 "Sending
Gate". Default OFF.

**Next, in order:**
1. **In-game verify Portal (user).** Restart ERR, toggle `World - Portals`; grep `[LOOTDISK] ... Portal
   markers (AEG099_510 bound to warp template 90005605; N entities harvested, M LOD-dup collapsed)` —
   expect ~23 gates at real sending-gate spots (Four Belfries, Siofra, Leyndell, DLC, …). On pass, merge
   `feat/mapgenie-portal` to master.
2. **Landmark GLYPHS — DONE (2026-07-02, `feat/per-marker-native-glyphs`).** Single-iconId
   categories landed first (`category_gpu_iconId`); then the per-marker plumbing closed the rest:
   `Marker.map_icon_id` = the row's own WMPP iconId, set by `push_marker` for `Source::Live` only
   (bosses + landmarks — Baked/DiskMSB loot iconIds are item/massedit ids, never map glyphs, stay
   0); the renderer's MP_ID tier prefers it over the per-category id. Net: ALL 16 landmark
   categories (incl. the Dungeon/LegacyDungeon/parity unions) now draw their native per-site
   glyph, and on ERR-less installs bosses (41/67) get a native glyph too (ERR keeps the
   name-keyed `MENU_MAP_ERR_Boss` first). Still open: Portals have no known SB_MapCursor glyph
   (check `AEG099_510` sometime); the F1 "icons replaced" counter (`category_is_gpu_native`)
   doesn't know about per-marker resolution yet — cosmetic.
3. **Rest of GROUP 2 — recon done, NOT quick wins. See `docs/re/windows_group2_landscape_re_findings.md`.**
   Portal was the clean one *because* it had a harvestable EMEVD template (`90005605`). The rest do NOT:
   - **Elevator / Smithing Table are ObjAct-bound, not EMEVD-template-bound.** Anchors found:
     Smithing Table = ActionButtonText 7030 / ActionButtonParam **6250**; Elevator = "Descend" 3301 /
     ActionButtonParam **5010**. But those ABP ids do NOT co-occur with their assets in EMEVD args
     (`_probe_g2_actionbtn.py`), and no candidate AEG model matches the counts (`AEG099_630` = 235 broad
     placements, not 40 lifts). Next step = an **ObjActParam/AssetObjActParam** parse (ObjAct row whose
     button=5010/6250 → MSB assets carrying that ObjAct), a new param path per category — bigger than Portal.
   - **Hidden Passage** = hit-detected illusory walls, NO action button → no static signal (hardest).
   - **Wandering Mausoleum** = dynamic moving entity (hard). Martyr Effigy = already `WorldSummoningPools`;
     Dragon Shrine folds into Churches; Landmark(172) = editorial → skip.
   Recon artifacts: `tools/_probe_g2_templates.py` (template→model map), `_probe_g2_actionbtn.py`.
   NB: offline SoulsFormats probes now need temp files in the REPO dir (`os.path.abspath('.')`), not
   `%TEMP%` — Defender started denying `%TEMP%` writes mid-session (`WinError 5`).
4. **DONE — Farmable.** `WorldFarmableCollectible` ("Loot - Farmable Drops") shipped + ERR-verified
   (`[LOOTDISK] … 70 farmable-notable`): respawning enemy drops of notable mats (Smithing Stones /
   Golden Runes / Gloveworts), all-8-slots scan (notable item is in slot 2), ~70 markers, off by default.
   `WorldFarmableEnemy` DROPPED (floods, no boss filter). Tuning knobs (notable set / per-item icons /
   dedup granularity) documented in `docs/memory/features/mapgenie-landmark-categories.md`.

## ⚠️ IN PROGRESS — baked-data → runtime/disk migration (build_pipeline.py deletion is the END state)

Authoritative plan: **`docs/plans/baked_data_full_removal_plan.md`** (full inventory + 6-phase
sequencing). `build_pipeline.py` can NOT be deleted yet — it's the LAST step (Phase 5), not the
first: it still generates authored tables with no runtime source (`category_exceptions`,
`name_aliases`, `world_feature_models`, boss list, region tables), and `build.bat` calls it 3×.

**Landed so far:** Phase 1 (dead `goblin_enemy_names` bake removed, enemy-drop labels now resolve
mod-agnostic via `NpcParam.nameId → GetMessage`) — merged, deployed, in-game verified on ERR. Phase
A regen (all 4 profiles now `MAP_ENTRY_COUNT 0`, non-ERR DLLs rebuild clean via clang/ninja) — done
at the build level; **the in-game vanilla sanity-check is still the one open item** (mod-agnosticism
can only be proven in-game on a non-ERR install — deploy `build-vanilla/MapForGoblins.dll` to a
vanilla install to close this). `.MASSEDIT` chain proven fully dead (not just baked=0) — the 14
dead `generate_*_massedit` stages + `goblin_massedit.{cpp,hpp}` (orphaned, never compiled) deleted.
`item_icon_table.json`'s baked category-exception override deleted + recovered LIVE via
`EquipParamGoods.sortId` (`goods_sort_id()`, `+0x20`, cross-checked vs `goodsType@0x3e`) —
in-game verified on ERR (every split repopulated correctly). `grace_position_index` bake dropped
(was already offline-only/dead-to-DLL, no in-game check needed).

**Remaining follow-ups (not blocking, note for whoever does Phase 5):** Reforged item families +
a few DLC key items (goods ids 2008025-2008037) still fall to the "Loot - Crafting Materials"
catch-all (colliding in-cell sortIds) — need dedicated `sortId` rules or accept the catch-all. The
offline mirror (`tools/taxonomy_classifier.py`, `_validate_taxonomy_map.py`) still applies the
deleted exceptions — resync or retire. `generate_loot_massedit` still emits a now-unread
`.MASSEDIT` alongside its live JSON — drop that emission when convenient.
**DONE 2026-07-01:** the tracked dead `.MASSEDIT` artifacts (`data/massedit` + `data/massedit_generated`,
113 files / ~9.9 MB) were purged + gitignored — the chain is proven dead (no runtime/pipeline consumer;
category-test tools read `regulation.bin`/params + `items_database.json`, not `.MASSEDIT`, so this is
safe for the pending MapGenie-coverage work). `generate_loot_massedit` still (re)writes
`data/massedit_generated` locally each run — now ignored, not tracked.

`goblin_name_aliases_en` migrated + bake DELETED (2026-07-01, `feat/name-aliases-runtime`): F1 English
search aliases now resolve live from the active install's `msg/engus/*.msgbnd.dcx` off disk
(`src/worldmap/name_fmg_en.cpp`; two-pass loose-mod-wins / packed-vanilla-fills; FMG-v2 group-table
lookup keyed on marker `name_id`), replacing the ERR-frozen table. Offline 2754/2756 vs the old bake;
**IN-GAME verified on ERR (cross-language) AND vanilla** via the me3 CLI (`[NAMEEN] 9877` vanilla names,
≠ ERR's count → reads the active install — see `docs/memory/tooling/me3-cli-nonerr-launch.md`).

**Next session — pick the next baked artifact to eliminate** (easiest→hardest, per the plan's
inventory): `goblin_tile_tabs`/`goblin_major_regions` (real + identical on all 4 profiles → dedup into
`generated_shared/`, pure housekeeping); `goblin_region_anchors`/`goblin_name_regions` (assess vs
`WorldMapPointParam`+`WorldMapPlaceName`); the icon atlas (baked overlay atlas, the prime-directive
example, biggest remaining item — see "Baked-atlas removal" below for why it's deferred). The regen
pipeline + all 4 profile builds are runnable on the Windows box (clang/ninja), so the confirm loop
is doable there.

## Baked-atlas removal — audited, DEFERRED until native coverage widens

Gate before deleting the baked overlay atlas: prove which categories actually need it per mod. The
`[ICONTIER]` census tool (`map_renderer.cpp`) tags each icon's resolve tier (mp_name/mp_id/item/
rep/atlas/circle) and tallies per draw pass — kept in the tree for re-auditing. Last audit (ERR vs.
vanilla, unioned since the census is per-view): **~15 categories still depend on the baked atlas**
(vanilla: Hostile NPC, Spirit Springs, Stakes of Marika, Kindling Spirits, Interactables, Spiritspring
Hawks; ERR: Cookbooks, Crystal Tears, Consumables, Scadutree Fragments, Pots-n-Perfumes,
Bell-Bearings, Crafting Materials, Golden Runes Low, Rune Arcs; `World - Maps` hits `circle`, no
glyph anywhere) — **verdict: gate NOT passed, keep the atlas.** To re-run: open the map ~5s on ERR
and on a vanilla launch (`internals/modengine/./bin/me3 launch -g eldenring -e "<steam exe>" -p
vanilla.me3`), grep `[ICONTIER]` both times, diff — native/rep in ERR but atlas/circle in vanilla is
what removing the atlas would regress. Follow-up not yet investigated: several loot categories that
hit `atlas` instead of `item`/`rep` are lot-backed treasures where per-item resolution runs but
yields `item_icon_id=0` — a one-shot debug log in `push_marker` dumping `lotId`/`lotType` + the
resolved key would disambiguate "fundamentally iconId=0" from "a fixable lot-resolution gap".

## OPEN — deferred for later

- **Lag-spike hunt, `refresh.collected.read_wgm` — ROOT-CAUSED + FIX LANDED 2026-07-03.** Cause: the
  CSWorldGeomMan RB-tree walk in `read_wgm_snapshot()` (`goblin_collected.cpp`) fired **~8-12 tiny
  `safe_read` (RPM) calls per node** — separate `get_is_nil`/`get_left`/`get_right`/`get_parent` +
  `block_id`/`block_data`/vec reads, EVERY refresh regardless of the instance cache. Under Wine each
  RPM is a wineserver round-trip and wineserver is a GLOBAL serialization point, so the per-refresh
  RPM burst on the refresh thread contended with the render thread → the frame [SPIKE]. Fix: bulk each
  RB-node header (0..0x30) in ONE RPM (`read_rb` helper, mirrors the `[FIELDINS-B]` diag pattern right
  below it) + bulk the block_data vec pointers → ~2 RPMs/node instead of ~8-12 (successor navigation
  reuses the already-read fields). Behavior-preserving (same offsets/traversal). Built clean +
  deployed. **IN-WORLD RESULT (real session 2026-07-03 04:49, fixed DLL): PARTIAL.** Steady path
  fixed — `read_wgm` AVG dropped to 0.04-0.10ms — BUT read_wgm STILL SPIKES 2-3ms (~33x, 4x/session).
  Those remaining spikes are the **cache-MISS path (new tile loads)**, NOT the steady tree walk: on a
  fresh tile, read_wgm re-reads EVERY geom instance's chain — geom_ins header + msb_part header + name
  = **~3 RPMs per instance** for all instances BEFORE the AEG099/AEG463 family filter drops the noise
  (hundreds × 3 RPMs = the burst). The tree-walk fix didn't touch that path.
  **NEXT for read_wgm spikes:** (a) budget the cache-miss resolve per refresh (cap K new tiles /
  N instances per call, defer the rest — CHECK the downstream graying contract first so a deferred
  tile doesn't flash wrong-collected); (b) land the [GEOMPROBE] endgame — AOB-pin the game's own O(1)
  collected getter (`goblin_collected.cpp:543` already arms DR0 for it) to replace the RPM snapshot;
  (c) a cheaper per-instance family discriminator than the name-chain (needs RE of a type/model field).
  **BIGGER FISH REVEALED:** the DOMINANT frame hitch this session was **`present.overlay_total`
  (20 spikes, up to 24ms)** + `render.minimap` — RENDER-side, unrelated to read_wgm; higher-value
  target if the goal is "stop the felt lag". (Cosmetic nit still open: the spike-ratio display divides
  by a near-zero baseline for quiet timers — floor the avg when touching that code.)
- **`present.overlay_total` spike — RESOLVED 2026-07-03: our overlay is NOT the lag (WONTFIX).** The
  full clean-exit SESSION REPORT settled it: `present.overlay_total` = avg 0.45ms, **1.11% of wall**,
  5 spikes/1048 frames. The newframe 28→22ms max localized (via the `present.nf_dx12`/`nf_win32`
  split) to **`present.nf_dx12` max 19.5ms = a ONE-TIME cost** (count 1048, total 20ms ⇒ one 19.5ms
  frame): `ImGui_ImplDX12_NewFrame` creating the font-atlas GPU texture on the first overlay frame.
  **The XInput/gamepad hypothesis was DISPROVEN** — nf_win32 stayed tiny (good call splitting before
  fixing). Nothing to fix here: it's a one-frame startup upload. **The REAL frame lag is game-side:**
  `present.frame_wall` max **883ms, 9 spikes** (= the game's own frame — area loads / transitions,
  ~30ms avg ≈ 33fps), NOT our code. Only marginal in-overlay item left = `render.worldmap.markers`
  max 49ms (one map-open-build frame, avg 1.4ms, 66 frames) — rare, low value. The sub-timers
  (`present.iconbatch/imgui_render/submit/newframe/nf_dx12/nf_win32`) are QUIET (spike/summary only,
  ~free) — LEFT IN as the standing overlay-perf breakdown. Original instrumentation note below.
  --- INSTRUMENTED 2026-07-03 (historical): The dominant
  frame hitch in the 04:49 in-world session (20 spikes, up to 24ms), living in the un-sub-timed
  parent−children gap of present.overlay_total (`hk_present`, `goblin_overlay.cpp`). Split that hole
  into 3 sub-timers (deployed): **`present.iconbatch`** (`flush_item_icon_batch`'s GPU fence WAIT —
  CPU blocks on GPU, frame-variable = PRIME SUSPECT), `present.imgui_render` (ImGui::Render draw-data
  build), `present.submit` (RenderDrawData vertex upload + ExecuteCommandLists). **NEXT: play a
  session, grep `[BENCH]`/`[SPIKE]` for which of the three spikes.**
  **LOCALIZED 2026-07-03 (clean-exit summary):** NOT iconbatch/imgui_render/submit (all stayed quiet).
  The `dump_report` summary pinned it: `present.overlay_total` avg 0.55ms / max 36ms / **5 spikes per
  914 frames = 0.5% (RARE, avg healthy)**; the big front-matter hit is **`present.newframe` max 28ms**
  (total 38ms/914 ⇒ ~ONE 28ms frame — `overlay.init.atlas` is only 3.9ms so try_upload_atlas is ruled
  out → it's ImGui NewFrame). Some present spikes are just `render.minimap` (child) bleeding through.
  **Prime suspect for newframe: `ImGui_ImplWin32_NewFrame` polling XInput every frame**
  (`NavEnableGamepad` set, goblin_overlay.cpp:1214; vendored Win32 backend calls `XInputGetState` each
  NewFrame; under Wine stalls ms on an absent/just-refocused pad — cf. "stale XInput after focus
  regain" ~L195). **Split `present.newframe` → `present.nf_dx12` / `present.nf_win32` (deployed)** to
  confirm next session (clean EXIT for the summary; trigger via focus-change / (re)connect a pad).
  **IF confirmed + recurs:** feed gamepad nav to ImGui from the repo's OWN XInput hook
  (input_gamepad.cpp) + compile backend `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD` to kill the redundant poll.
  **IF ~one-off** (data leans this way — 1 frame/914): a rare focus-event hitch, likely NOT worth the
  gamepad-nav-regression risk. Decide from the nf_win32 vs nf_dx12 split.
- **Map-exit input softlock.** Root cause for the general "soft key lock at screen edge" turned out
  to be **external** — Deskflow (cursor-sharing KVM), not ER or this mod; fix is Deskflow-side, see
  `docs/re/windows_input_softlock_re_prompt.md`. The F1-mouse-dead half of the original report was a
  separate, already-fixed bug (see the Alt+Tab fix chain in `changelog.md`). Low priority to revisit.
- **Item-name localization (FR/EN) — DONE 2026-07-01, the bake is gone.** (Superseded the earlier
  "keep the bake" call.) The cross-language English alias no longer ships baked: it's read live from
  the active install's engus msgbnd off disk at init (`name_fmg_en.cpp`). The feared ~10 MB Oodle cost
  is a one-time init decompress (~1.5 MB retained index), not per-frame — acceptable, and it's the only
  mod-agnostic option (baked shipped ERR's names into every profile). See the baked-data section above.
- **Is non-ERR/vanilla a hard support target?** Still an open policy question — decides whether the
  baked atlas (and similar ERR-leaning bakes) can eventually go fully or must stay as a permanent
  vanilla-compat net.

## Known standing gotcha — double-DLL-load, not a code bug

If both `MapForGoblins.dll` (ERR build) and `MapForGoblins_vanilla.dll` are present in the mods
folder, BOTH load into the same process → doubled everything (double ImGui draw, double
MsgRepository PlaceName patch producing `?PlaceName?`, double hook installs, discovered-grace
markers appearing to vanish) — confirmed by log diff, not a real code bug in any of the affected
systems. **Fix (immediate):** only ship/deploy ONE DLL — the launcher should load `MapForGoblins.dll`
only (renaming it does NOT disable the mod; the launcher falls back to the stale `_vanilla.dll` — to
truly test mod-off, remove BOTH). **Fix (strategic): DONE 2026-07-02 — the single-DLL migration
retired the per-profile variants; new packages ship one `MapForGoblins.dll` for every install, so a
fresh install can no longer double-load. (Stale old `_vanilla.dll` files in EXISTING installs can
still double-load until removed — the named-mutex hardening below still has value.)** **Hardening TODO, not yet implemented:** a named-mutex check at init
(`CreateMutexW`) so a second instance bails before installing any hooks/ImGui/PlaceName patch and
shows a clear on-screen "double load detected, check your launcher config" banner instead of silent
double-draw corruption — currently the failure mode is confusing, not caught.

## Key findings / non-obvious facts (icon/glyph work)

- The 7 mod-added POI categories (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have NO ERR-custom glyph — their massedit iconIds
  (374+) point to glyphs absent from all current menu files (numeric glyphs cap at 261). Recover via
  a real `SB_MapCursor` glyph where one fits (e.g. summon→89), else circle.
- `MENU_MAP_ERR_*` (boss/grace) are ERR-only names; on non-ERR they won't resolve → circle if the
  baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
