# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping (2026-07-03, done):** file had grown to 1254 lines, mostly narrative for work already
merged, changelog'd, and in-game verified. Compacted to genuinely live/in-progress work, open
questions, and standing knowledge (gotchas, deferred decisions, non-obvious facts) not captured
elsewhere. History for anything not below: `docs/changelog.md` first, then `docs/plans/*.md`,
then `docs/re/*.md` (RE findings) and `docs/memory/`.

## ⇒ SESSION WRAP 2026-07-04 — Virtual World Map (mod-owned page) + test orchestration

Big session. Two tracks landed (all committed, in-game verified where noted; local master ahead of origin):

**1. Virtual World Map — the mod-owned map page (World Virtualization vision #1).** Slices A→D + C1/C3 all
DONE + live-verified (see the "In-Game World Editor" item's virtual-page block below for detail):
A canvas (pan/zoom/grid), B markers (6837 ER markers → Lands Between silhouette), C1 world registry
(`goblin_virtual_world`), C3 bundle persistence (`virtual_worlds.toml`, 2-cold-boot verified), D decouple
from F1 + open via the game MAP KEY. RPCs: `vmap`/`vworld`/`f1_tab`. **NEXT on this track (pick one):**
- **ENDGAME phase-1a = load ER's real map ART** onto the canvas → `docs/plans/map_tile_loading_plan.md`.
  **✅ sub-slices 1a+1b DONE 2026-07-04** (format cracked + validated offline AND in-game): BHF4 parser
  lives in `src/worldmap/maptile.{hpp,cpp}` (`parse_bhf4`/`load_archive`/`extract_dds`) + `maptile_probe`
  RPC; offline tool `tools/tpfbhd_recon.cpp`. In-game recon of the packed `71_MapTile`: **28469 tiles,
  256×256**, extract chain works; 4 dimensions (M00 overworld / M01 underground / M10 DLC / M11 DLC-ug),
  LOD pyramid L0(fine)→L3/L4(coarse) — full per-level counts in the plan.
  **✅ sub-slice 2 DONE + LIVE-VERIFIED 2026-07-04:** the vmap canvas now draws REAL ER map ART. RPC
  `vmap tile <needle> [rect]` / `tiles_clear`; `maptile::extract_named` + `panel_virtual_map` service the
  load on the render thread (`create_tex_from_dds_mem`, like panel_dev_icons' on-click load), cache
  {SRV, world quad}, and `dl->AddImage` under grid+markers. A 2×2 M00_L0 block rendered as seamless ER
  terrain (screenshot-confirmed under Proton).
  **✅ sub-slice 3a DONE 2026-07-04 — live map-space→world transform (the Convergence-trap-safe core).**
  The vmap places tiles in the SAME world frame as markers, derived 100% LIVE via `worldmap_probe::project`
  (project every overworld marker → robust MEDIAN offset, fixed ±1 slope since converter scale is live=1):
  result EXACT `worldX=mapU+7040, worldZ=−mapV+16512`, ground-truth-verified (marker world(10138,10046) ↔
  engine map(3098,6465)), NO hardcoding. RPCs `vmap tiles_lod <dim> <lod> [cap]` (reads archive once,
  center-out, cap for the SRV limit) + `vmap view <camX> <camZ> <zoom>`. Tiles land in the correct region
  (co-located with markers, live-verified). **Testing gotchas found:** `set rpc_auto_idle false` before
  scripted input (auto-idle SUSPENDS it when a human is at the PC); the map cursor/VM only publishes once
  the map view is NON-static (pan/zoom); the ER world map opens with the **`m`** key on this install.
  **NEXT (3b/3c/decode):** (1) **`{suffix}` decode — CRACKED (static Ghidra, 2026-07-04,
  `docs/re/windows_worldmap_tile_placement_re_findings.md`).** NOT a variable-depth quadtree: every tile is
  a **constant 256×256 map-space quad** (`DAT_143b37d00..d0c={0,0,256,256}`) on a **uniform SPARSE grid**.
  Decode: `suffix = 8·morton(subX,subY)` in a **64×64-cell block** → `gridX=col·64+subX`,
  `gridZ=row·64+subY`, `rect = ((gridX−gridXbase)·256,(gridZ−gridZbase)·256)+(0,0,256,256)`
  (`gridXbase/gridZbase` live from the converter). Verified: `morton(52,38)·8=0x69c0` = block-02_03 max.
  No `WorldMapTileParam` (pure name-decode). LOD = per-LOD layer stack, zoom-gated (`WorldMapTiledLayer`
  array at mgr+0x390, stride 0x110). Implement `decode_suffix`+`tile_map_rect` in `maptile.cpp` (ref C in
  the findings). **Runtime to confirm on Linux:** Morton axis-order (even-bit = col-axis or row-axis?) +
  `W=64` holds for the active archive, via one screenshot vs. a known marker. (2) **SRV recycling** —
  256-cap no free list; even coarsest M00_L3=561>256. (3) byte-range reads (extract reads whole 1.26 GB
  .tpfbdt). Then the full seamless overworld renders under the markers.
- Missed design items (captured in `docs/plans/virtual_world_multi_world_design.md`): **GAMEPAD** for the
  vmap canvas (stick→pan/zoom + reticle — add to every vmap slice); the real feature gaps = **clock /
  blue click-marker / custom beacon** (rest of ER's map is cosmetic; grace = fast-travel = make-or-break).
  **✅ sweep coverage DONE 2026-07-04:** `test_vmap.py` (single-boot SWEEP: open/close/group/fit round-trip,
  10/10), `test_vworld.py` (2-boot persistence: create+2 markers+save → cold boot → `vworld list` shows
  `(mk=2)` restored FROM DISK, 8/8), `test_world_bundle.py` (2-boot, the TOML-fix proof, 4/4). Added a
  marker-count readout to the `vworld list` RPC (`[id]name(mk=N)`) to make the reload verifiable.

**2. Test orchestration — regressions are no longer phantoms.** `mfg_session` persists PASS/FAIL to
`tools/rpc_tests/results.jsonl` (gitignored) + inline regression flag; `check_regress.py` scans + regenerates
git-tracked `tools/rpc_tests/STATUS.md`; `run_all.py` = AGGREGATED sweep (single-boot tests share ONE game
boot via a `SWEEP` marker → 9-test suite = 4 boots) → ledger → check_regress → gated exit. Nightly LOCAL cron
line documented (user adds to crontab; **Steam must be up** — `steam -silent`). `.vscode/tasks.json`
(git-tracked) = one-click build/deploy/test. Paths now **env-driven** (`.env`/`.env.local`, ERR_ROOT+GAME_DIR;
`.env.local` gitignored). `assets_probe` RPC + `test_assets.py` = path-loading guard (loose/packed/MISSING per
install shape). Open follow-up: `world_bundle` TOML load is latently broken under Proton (migrate to
TOML_EXCEPTIONS 0 like virtual_world/custom_items — see the open item below).

**Also this session (RE):** geom-spawn ADD standalone-ctor confirmed DEAD END (builder hangs, streaming-
welded); `spawn_clone` neutralized; real ADD = the asset-request path (pivot 2, Windows RE) — see the geom
placement item below.

## ⇒ RESUME HERE — sidecar Phase 2 (clean-save item strip/reinject): bracket is LIVE, cap-oracle E2E is next

**Where things stand:** the whole-slot save serialize is found and pinned — `SERIALIZE_FN`
@ `er+0x67dc00` (`FUN_14067dc00`, GameDataMan-xref ∩ DLOutputStream-writer, save-specific/synchronous/
direct-called; full RE in `docs/re/windows_save_serialize_re_findings.md`). `install_save_hook()` is
retargeted to it, observer-confirmed firing on the save worker thread (2 fires/save, correct AOB), and
the strip@entry/reinject@exit bracket is **wired + live** (`be7b212`): `strip_items()` →
`g_orig_ser(..)` → `reinject_items()`, synchronous, guarded by `g_in_serialize`,
`kItemStripReinjectWired=true`. `test_sidecar` 5/5 passes with the bracket live, no crashes.

**NOT yet proven:** that the bracket actually produces a clean on-disk vanilla save. Recipe: grant a
reserved-id item live (`give_item`) + register it (`sidecar additem`) → trigger a real game save →
reload with the `.mfg` `[items]` emptied → item must be GONE from the vanilla save. That assertion
needs an automated **`goods_count(id)`** read — now SOLVED:

**✅ `goods_count` FOUND + IMPLEMENTED 2026-07-03 (Windows-Ghidra, `docs/re/windows_goods_count_re_findings.md`).**
The blind 2-level `goods_diff` failed because the held qty is neither inline next to the id NOR ≤2 hops
out: ER uses GaItemHandle indirection AND the held list is a **two-segment split list three hops from
GameDataMan**. Ghidra (`D:\ghidra_proj2\ER`, new `tools/ghidra/find_goodscount.java` + `query.java`)
pinned the full layout: **`GameDataMan+8 → +0x2B0 EquipGameData → +0x158 EquipInventoryData` (carried)**;
segments (seg1_cap@+0x1C, seg1_base@+0x50, seg2_base@+0x40, last_index@+0x80), node stride `0x18`, node
`{handle@0 (0⇒empty), itemId@4 (0x40000000|goodsId), quantity@8}` (qty offset cross-checked via decrement
path `FUN_14024bfe0` + accessor `FUN_1407127a0`). Delivered **option (3), the direct read-only walk** (no
game call, no thread/save-timing risk) as `goblin::inventory::goods_count(id)`
(`goblin_inventory.{hpp,cpp}`, RPM-guarded, reuses `equip_game_data()`) + RPC `goods_count <id>` (reports
`err not in-world` vs a real `n=0`). Builds clean (clang-cl). Callable fallbacks recorded
(`FUN_14024c460`/`…c560` by-id finders).

**✅ goods_count offsets LIVE-VERIFIED 2026-07-03 (Linux/Proton).** Cross-built + deployed, then
`tools/rpc_tests/test_goods_count.py` (GameSession cold-boot → load save → grant/read) went 6/6:
fresh id `0x40003bed` reads `0→1→2→3` on repeated `give_item +1`; held id `0x40003bec` `7→8→9→10`.
Read tracks live held qty per-id, in-world. **Caveats found (give_item, NOT the read — full note in
`windows_goods_count_re_findings.md`):** AddItemFunc is ADD-ONLY (negative qty = no-op — the old
"−7 → 0" verify recipe was wrong; removal needs the remove path); `qty≥~5` clamps to the ~1000 stack
cap (grant N via N× `+1`); grants are live-inventory only, not persisted until a real save (fresh id
re-reads `0` after reboot → regression is idempotent).

**✅ Variant A clean-save CLOSED 2026-07-03 (Linux/Proton, E2E 4/4).** `tools/rpc_tests/test_custom_item.py`
(two cold boots: grant+additem+warp-save → empty `.mfg [items]` → reload) proves a registered custom
item does NOT survive in the vanilla `.sl2` once the `.mfg` stops re-granting it: boot-2 `goods_count==0`.
Three fixes made it work (the original bracket was a silent no-op):
- **Real strip (not `give_item(-qty)`).** AddItemFunc is add-only, so `strip_items()` now zeroes the
  matching EquipInventoryData node directly (`inventory::strip_goods()` — snapshot 0x18 bytes, write
  `handle@0=0`+`qty@8=0`; the exact decrement the game's `FUN_14024bfe0` does) and `restore_goods()`
  writes the bytes back the instant the serialize returns. The serializer honors the zeroed slot.
- **`WriteProcessMemory`-to-self silently FAILS on the inventory pages** (qty stayed 6 after a WPM
  strip) — a **direct in-process store under SEH** (`write_dw`/`write_bytes`) sticks. Use those, not WPM.
- **Idempotent reinject.** World-enter `reinject_items()` now grants only the missing delta
  (target − held) via the exact `give_item(+1)` primitive — a warp/area re-enter (item still live)
  grants 0 instead of inflating +1/save; a cold load (item stripped from the save, held=0) grants full qty.
Dev RPC `strip_test <id>` validates the strip round-trip WITHOUT a save (before→strip→0→restore→before).
Variant B (reserved-id item tolerated in the `.err`, no serialize hook) remains the zero-RE fallback.

**NEXT:** Gap C GRANT for arbitrary custom items can now build on this proven sidecar item (the Gap H
"don't dirty the `.sl2` until strip proven" contract is satisfied). Caveat still open: `give_item(+N)`
single-call is unreliable for N>1 (caps ~1000) — grant N via N× `+1` (reinject already does).

**Infra note (corrects stale memory):** a background Claude job CAN boot ER for a self-contained RPC
run — the missing piece was **Steam must already be running** (me3's `require_steam` aborts otherwise:
`ERROR require_steam: Steam is required to run this game`). Start it headless once with
`steam -silent` (auto-login persists, daemonizes, survives across tool calls), then `GameSession`
launches me3 as its in-shell child and kills the game at exit. See `mfg-rpc-driver-hardening.md`.

## Open / next items

- **NEW map entry for MFG markers — SPIKED 2026-07-04 (`docs/re/worldmap_new_page_spike_findings.md`).**
  User goal: a new map ENTRY so MapForGoblins markers show on a custom "dev world". **Spike verdict (this
  CORRECTS the earlier "lighter data-layer" guess):** a NATIVE new page is its OWN unsolved WRITE frontier,
  NOT a light param task — the page SET is a converter array built live in `CS::WorldMapViewModel` (ctor
  `FUN_1408855b0`, count `WorldMapViewModel+0x280`=8, page-byte table `DAT_142ad82f8=[00 01 0a]`) from
  regulation; no injection path is RE'd, plus menu-tab registration + a tile-art sheet in the ERR `.gfx` are
  all write-unknown (adjacent to the MSB wall). The whole READ side is solved (projection/converter fields/
  page switch), the WRITE side is not. **⇒ Achievable path = a MOD-OWNED virtual page, NOT a native one:**
  the overlay already draws in the backbuffer and only needs the open group-id to cull (`map_renderer.hpp:4`),
  so MFG can own a synthetic group id (≥100) + a mod-defined projection (origin/scale/bias) + a mod-drawn map
  surface (bg image + pan/zoom, opened via a mod toggle) with markers tagged to it — 100% mod code, no engine
  write, Linux-doable, and it IS World Virtualization vision #1 (a custom world = a bundle whose map is this
  virtual page). Reserve native-page registration as a far-frontier item (only if the world must be a real
  in-game map TAB). **User CONFIRMED mod-owned page (2026-07-04).** Slices:
  - **✅ SLICE A DONE 2026-07-04 — the virtual-map CANVAS.** `src/overlay_panel/panel_virtual_map.cpp`: an
    ImGui window "MapForGoblins — Virtual World Map (WIP)" with a world-space canvas (drag=pan,
    wheel=zoom-about-cursor), a snapped reference grid + origin cross + grid-step legend, and a mod-defined
    world→canvas projection (`w2s`/`s2w`; cam in world units, zoom px/unit). Drawn as a sibling of the F1
    panel (compact+full). Toggle: Dev-tab button **and** `vmap 0|1|toggle` RPC (+ `overlay_api::
    virtual_map_set_open/is_open`). Live-verified on Proton (`vmap 1` → window+grid+origin render, alive).
  - **✅ SLICE B DONE 2026-07-04 — markers on the canvas.** The selected group's live markers project onto
    the canvas as colored dots (by `m.color`), with a group selector (Combo) + `Fit`-to-markers + a marker
    count readout. Live-verified: base-overworld = 6837 markers forming the recognizable Lands Between
    silhouette; `vmap group <0-3>` / `vmap fit` RPCs drive it. On-canvas ICONS (vs dots) = a follow-up (needs
    a draw-list icon helper: resolve tex+uv like the census, `dl->AddImage`). **NOTE:** the vmap currently
    draws INSIDE `draw_panel` so it needs F1 open — slice D must decouple it (draw independent of g_show) for
    the M-open path.
  - **✅ SLICE C1 DONE 2026-07-04 — the WORLD model + registry.** `src/goblin_virtual_world.{hpp,cpp}`:
    a mutex-guarded registry of custom worlds (`{id, name, originX/Z, scale, markers[]}`) + an active-world
    id (0 = Base ER). The vmap gained a **World selector** (Base ER → live ER markers by group; a custom
    world → its OWN markers in its own coordinate namespace) — collision-free by construction (each world =
    its own coord space, framework owns ids/placement). RPC `vworld create|marker|active|list|clear`.
    Live-verified: created DevWorld, added 11 markers (a 3×3 grid + diagonals), `vworld active 1` → the vmap
    drew exactly those 11 in the world's own space (centre 1000,1000), switchable back to Base ER.
  - **✅ SLICE C3 DONE 2026-07-04 — bundle persistence.** `goblin_virtual_world` now saves/loads the
    registry (+ active id) as `<mod>/virtual_worlds.toml`; boot-loaded via `dllmain init_virtual_worlds`
    (`vworld::load_boot`) right after `world_bundle`. RPC `vworld save|load`. **E2E-verified across TWO cold
    boots:** boot-1 create "Persisted" + 5 markers + `vworld save` → boot-2 boot-load restored it (active=1,
    5 markers rendered from disk). **⚠ Required the TOML fix (`#define TOML_EXCEPTIONS 0` + `parse_file`) —
    the exceptions-ON `parse(string)` path returned an EMPTY table (0 worlds) under Proton; see the corrected
    `docs/memory/tooling/toml-parse-file-proton-bug.md`. This also flags `goblin_world_bundle` as latently
    broken (same disproven pattern) — migrate it + add a reboot test (new open item below).**
    **Remaining C:** C2 = tag EXISTING mod markers to a world (synthetic group ≥100 in `marker_layer.hpp`).
    Per-world origin/scale projection is wired (fields) but identity so far.
  - **✅ SLICE D DONE 2026-07-04 — decoupled from F1 + opens with the game MAP KEY.** The vmap now draws on
    its OWN per-frame entry (`draw_virtual_map_entry` + a `call_draw_virtual_map` loader trampoline / the
    `MFG_DrawVirtualMap` hotreload export), called UNCONDITIONALLY in the present loop — so it appears
    WITHOUT the F1 panel. **LIVE-VERIFIED: `vmap 1` with F1 CLOSED (`panel=0`) → the window renders
    standalone.** The M-trigger is wired in `draw_virtual_map`: on the `world_map_open()` rising edge with a
    CUSTOM world active (`vworld::active()!=0`) it opens the vmap, and closes it on map-close if IT opened
    it (a Dev-toggle vmap is left alone). **M-trigger not harness-verified** (driving the game map open needs
    the install's map keybind — impractical to guess-inject); it's simple code on the proven
    `world_map_open()` primitive, **user-verifiable by pressing M in a custom world**. **Follow-up:** the
    vmap is a movable WINDOW over the native map, not a full-screen replace — native-map suppression (draw
    the mod surface in the map's place) is a later polish.
  - **ENDGAME (total native-map replacement) — SCOPED + phase-1a format-CRACKED 2026-07-04, not built.**
    Vision: the mod map fully REPLACES ER's native worldmap (one UX, no marker clipping — the clipping win
    is FREE from a full-screen mod surface, no risky native suppression needed). Make-or-break = FAST-TRAVEL
    (warp-on-grace-click; primitive `warp` exists). **Phase-1a = load ER's real map ART onto the canvas** —
    fully scoped in `docs/plans/map_tile_loading_plan.md`: the DCX→DDS→GPU chain all EXISTS
    (`create_tex_from_dds_mem`, `dcx_decompress`, `tpf_find_texture`, the `read_item_icon_sheets` template,
    the img→resource offsets, `w2s` positioning); the ONE gap was the `71_MapTile.tpfbhd/.tpfbdt` archive —
    **format CRACKED: `BHF4` header (entry table {hash,size,offset}) + `BDF4` data, each entry = `DCX`→TPF→
    DDS (inner half already handled), so the only new code is a small BHF4 entry-table parser (SoulsFormats
    BXF4).** Two paths: RAM-harvest (resident `MENU_MapTile_*` via `force_load_file`, but SRV-256-cap +
    g_icon_repo-needs-a-menu wrinkle) vs disk-extract (in-game dvdbnd read — `dvdbnd_reader` is Windows-only;
    OFFLINE format-recon possible on the loose `00_Solo.tpfbhd`). **This is a real SLICE (DX12 SRV-cap
    streaming is the main engineering constraint), best as a focused fresh-context session.**
  - **Full architecture (collision / active-world / M-open / how ER's map works) = `docs/plans/
    virtual_world_multi_world_design.md`** (2026-07-04). Key decisions: the FRAMEWORK assigns position (not
    the player) — marker worlds get separate mod namespaces, walkable worlds get a reserved mapId
    (ER's dimension mechanism); active world = player mapId (walkable) or explicit bundle (marker); ER's map
    is BAKED `WorldMapTile` DDS sheets (overworld/UG/DLC = separate dimensions), so custom worlds supply
    their own image/grid. Walkable worlds also need the ADD-geom frontier (pivot 2, Windows RE).
- **✅ `goblin_world_bundle` TOML load FIXED + TESTED 2026-07-04.** Migrated to `#define TOML_EXCEPTIONS 0`
  + `toml::parse_file` (was the exceptions-ON `ifstream+parse(string)` path virtual_worlds C3 DISPROVEN).
  New `tools/rpc_tests/test_world_bundle.py` = the missing genuine save→reboot→load (boot-1 record 1 clone +
  1 set + `bundle save` → cold boot → boot-2 `bundle status` reads `clones=1 sets=1` FROM DISK): **E2E 4/4
  under Proton.** So ALL DLL TOML configs now use the only-working `TOML_EXCEPTIONS 0` path. NB the bundle
  lives in `<ERR_ROOT>/dll/offline/` (the DLL's own folder), NOT `mod/`. See
  `docs/memory/tooling/toml-parse-file-proton-bug.md`.
- **F1 category list → GRID LAYOUT (followup, not started).** The Markers-tab category list is a checkbox
  tree; with many custom worlds/categories it overflows into a long scroll. Replace with a GRID of
  icon-tiles (the category icon we just added as the tile, toggle visibility on click, checkmark/dim
  overlay for on/off), gamepad + keyboard navigable (reuse the already-enabled ImGui nav — same nav that
  drives the on-screen keyboard). Natural successor to the category-icons work (`draw_category_icon`).
  Pure ImGui slice, Linux-doable. Keep the per-category count badge + cluster toggle reachable (tooltip or
  a detail row on select).
- **In-Game World Editor (vision #2) — SLICES 1+2 DONE 2026-07-03.** F1 panel section "World Editor
  (live)" (`src/overlay_panel/panel_world_editor.cpp`): pick an AEG asset → it shows the loot item its
  MAP MARKER resolves to (live `aeg_pickup_lot`→`resolve_loot_item_textid`→`lookup_text_utf8`) → set that
  lot's `lotItemId01` to any goods id → `Refresh markers` → shows on the map. Wires the proven runtime
  primitives (`overlay_api::param_set_field` new bridge + `rebuild_markers`) to widgets. Slice-1 visually
  verified ("Lot 997230 → Bloodrose").
  **Slice 2 (2026-07-03): repoint-to-another-lot.** Panel now also sets the asset's `pickUpItemLotParamId`
  to a different EXISTING lot (non-destructive — leaves the shared lot alone), with a live preview of the
  target lot's slot-1 item before commit. Pure ImGui over already-proven bridges (`param_set_field` on
  `pickUpItemLotParamId` is the same write the RPC repoint used; live re-read via `aeg_pickup_lot` from
  slice 1). **✅ DEPLOYED + E2E-VERIFIED 2026-07-03 (Linux/Proton, 8/8)** —
  `tools/rpc_tests/test_world_editor_slice2.py` cold-boots ER, loads a save, and proves the exact panel
  write path: `help` returns the verb list; repoint asset 99030 (lot 900002000 → 997230) then
  `param_getf`==997230 AND `loot_at` resolves lot 997230 + 'Bloodrose' (textid 500020723); restore to
  900002000. Also proved the new `help`/`?` RPC verb. (Test caveat baked in: discover pickup assets by
  loot TEXTID, not name — many valid lots resolve an empty FMG name off this chain.)
  **Slice 3 (2026-07-03): per-slot re-skin — DONE + E2E-VERIFIED (12/12).** The in-place re-skin now
  targets any of a lot's 8 slots via a `Slot` selector, showing the selected slot's live item id. Added
  `ItemLotParam_map.lotItemId02..08` to the paramedit registry (offset-only, core-stable, `+0x00+(N-1)*4`)
  + an `overlay_api::param_get_field` name-addressed read bridge. `tools/rpc_tests/test_world_editor.py`
  (renamed from `_slice2`) round-trips lotItemId02 (0→424242→0) on top of the slice-2 checks.
  **Slice 5 (2026-07-03): CLONE a lot — DONE + E2E-VERIFIED (16/16).** A `Clone this lot` button
  (`overlay_api::param_clone` new bridge → `param_clone_row`) copies the current lot to a fresh row and
  pre-fills the repoint target; combined with the `refresh_markers` v2 LotReader reset (see the live
  marker regen item below), a cloned lot now resolves on the map. Test proves invisible-before /
  resolves-after refresh.
  **Slice 6 (2026-07-03): asset/item PICKER — DONE + E2E-VERIFIED (18/18).** New host module
  `goblin_world_editor.{hpp,cpp}` scans the live params into browsable lists — pickup assets
  (AssetEnvironmentGeometryParam rows with a real `pickUpItemLotParamId`, name via the loot chain) and
  named goods (EquipParamGoods, name via `lookup_text_utf8(id+500000000)`). Exposed through
  `overlay_api::we_scan`/`we_copy_assets`/`we_copy_goods` (+ POD `WEAsset`/`WEGoods`) and a `we_scan`
  RPC. The F1 `Browse (pick asset / item)` section (Scan button → client-side filter → click sets the
  Asset / New-goods-id fields). Live scan on a loaded save: 324 pickup assets, 5499 named goods.
  **Slice 7 (2026-07-03): SAVE edits as a world bundle — DONE + E2E-VERIFIED (24/24).** New host module
  `goblin_world_bundle.{hpp,cpp}` records the editor's edits (dedup: SET keeps last per param/row/field,
  CLONE unique per newId) and persists them as `<mod>/world_bundle.toml` (`[[clone]]` + `[[set]]` arrays,
  toml++). `apply_current()` re-runs them (clones first, then sets, then `reset_lot_reader`);
  `apply_boot()` re-applies the default bundle at startup (wired in `dllmain` right after
  `custom_items::apply`, before the first marker build → no LotReader reset needed at boot). Panel:
  `Save / Apply / Clear bundle` buttons + op count; RPC `bundle status|clone|set|save|load|apply|clear`.
  E2E: record clone+repoint → save → clear memory → apply-from-disk → asset resolves the cloned lot's
  item. **This is the first brick of vision #1 World Virtualization** (a swappable world = a bundle);
  remaining for #1: multiple named bundles + live swap (reset-to-base + apply + refresh) + per-world
  sidecar save context. **⚠️ GOTCHA found:** `toml::parse_file` returns an EMPTY table under Proton/Wine
  (silent, no throw) — but ONLY in the exceptions-ON config; read via `std::ifstream` +
  `toml::parse(string)` instead (world_bundle does). **custom_items.cpp CHECKED — NOT affected**: it
  `#define TOML_EXCEPTIONS 0` so its parse path works (re-verified `test_author_items.py` 1/1). No fix
  needed there. See `docs/memory/tooling/toml-parse-file-proton-bug.md`.
  **Next slices:** category select (weapons/armour/… beyond goods in the picker), and the World
  Virtualization multi-bundle swap. (`refresh_markers` v2 fully done.)

- **MSB WRITE — frontier #1, first probe scoped 2026-07-03 (`docs/re/windows_msb_placement_write_re_prompt.md`).**
  The keystone for "create new content" (custom mob/treasure placement, new map geometry). We READ MSB fully
  (both routes, `msbe::parse_msb`) but have NO write path. The probe is a cheap decision: RE the MSB→instance
  load path (static Ghidra — `CSMsbPartsGeom`/`CSMsbPartsMap` ctor) to learn whether the position is
  **snapshotted into the spawned instance** (⇒ resident-MSB writes are inert, the movable transform is on the
  instance — likely reusing the enemy/boss + FieldIns transform we already RE'd) or a live pointer; then a
  2-target live write test (instance transform vs resident MSB bytes) picks the layer. If "move an existing
  placement live" falls out, it's an immediate World-Editor slice (drag-a-placement). "Add new" then splits
  into a spawn-factory + tile re-stream follow-up. **Volet A DONE (static, 2026-07-03,
  `..._findings.md`):** the MSB position is snapshotted TWICE (`CSMsbParts` ctor `er+0xcee430` copies
  `+0x20` out of the blob → `CSWorldGeomIns` ctor `er+0x6c5900` builds its OWN transform at `+0x18` from a
  separately-passed world matrix) → **resident-MSB byte writes are provably inert.** The movable transform
  is the FD4 location module at `CSWorldGeomIns+0x18` (world matrix cached at `+0x44`), so moving needs the
  **setter**, not a flat poke. **`CSWorldGeomDynamicIns` (`FUN_1406b9880`, on factory `FUN_1406c5900`) is a
  movable geom class** = the vehicle for both move + add. **Setter FOUND (vtable-walk):** the world transform
  is set via **`vtable[0xd0]` (slot 26) `SetWorldMatrix(inst, mat4x4)`** on the FieldIns/geom instance
  (proven by two callers `er+0x6c9aa0`/`er+0x6e4210`; getter `FUN_1406c46e0(inst+0x18,&out)`, row-major 4x4,
  translation in last row). **NEXT:** the live move probe is now a direct vcall
  `(*(void(**)(void*,const float*))((*(void***)inst)[26]))(inst, mat)` behind a dev RPC — get `inst` from the
  geom manager (`DAT_143d7b0c0[+0x10]`) or the FieldIns registry; observe the object move. Then "add" = drive
  the Dynamic factory (`er+0x6b9880`) from a synthesized parts rec + transform (trace `er+0x6a7930`/`er+0x6adc80`).

- **Long-horizon vision bets — tracked in `docs/runtime_modding_framework_vision.md` "Future directions"
  (2026-07-03):** (1) World Virtualization — a FRAMEWORK feature: the framework holds N of its OWN worlds (each a data
  BUNDLE of param overrides + custom items + names + map/loot edits + flags + save context) and swaps the
  active one live over one shared base. NOT third-party-overhaul interop (the Convergence⟷ERR line is
  only an analogy). Missing = a bundle format + activation (reset-to-base + apply + `refresh_markers`) +
  per-world sidecar save context. Shares the primitives with #2,
  (2) In-Game World Editor (ImGui over the runtime primitives — the live-edit loop already EXISTS:
  `param_setf`/`param_clone`/`loot_at`/repoint/`lotItemId01`/`refresh_markers`; the editor is the panel
  wiring), (3) 3D model variants + reuse across worlds (asset/MSB frontier — needs an MSB-write path that
  doesn't exist; hardest/furthest). Not scoped; captured so they aren't lost.

- **Geom placement MOVE — CRACKED + LIVE-VERIFIED 2026-07-03 (`move_asset` RPC, 7/7).** The MSB-write
  frontier's "move an existing placement" half is solved: the transform setter is `vtable[0xd0]
  SetWorldMatrix(self, mat4x4)` on `CSWorldGeomIns`. `goblin_geom_move.cpp` picks a live geom instance
  (`goblin::collected::first_live_geom_instance()`, the WGM/CSWorldGeomMan walk — the FieldIns registry
  `[er+0x3d7b0c0]…` was EMPTY in-world) and vcalls it; `move_asset 0 100 0` moved the cached world matrix
  (`inst+0x220`, one float) by exactly +100 Y, game alive. Full RE + nuances in
  `docs/re/windows_msb_placement_write_re_findings.md` (setter writes the `+0x220` CACHE, not the `+0x18`
  module → verify via `+0x220`, not the getter). **Persistence CONFIRMED 2026-07-03** (`move_hold`/
  `move_read` RPCs): a cache-only write is DURABLE — `move_hold 0 100 0` then polling `+0x220` 10× over
  ~7s held at Y 83.38, no engine revert from the `+0x18` module. So no module-sync needed for a move to
  stick. **On-screen render CONFIRMED 2026-07-03 (user-observed live):** `move_all <d>` (mass move of
  every loaded geom instance; new `move_near`/`move_all`/`move_restore` + `list_live_geom_instances`) was
  watched live — props visibly moved. Automated screenshot pairs were defeated by First Step's scripted
  intro camera (pans between shots even after a 15s settle), so the confirm is the live observation + the
  byte-exact/persistent proof. **Move primitive is fully proven (static→live→durable→renders); it does not
  need `+0x18`/Dynamic.** **Remaining MSB-write hole: ADD a NEW placement — now SCOPED (static, 2026-07-03).**
  Traced the spawn drivers `FUN_1406a7930`/`FUN_1406adc80`: there is **NO isolated "spawn one geom" call** —
  they are the tile-streaming state machine over the loaded MSB resource, and instances are
  **placement-new'd into fixed-capacity BlockData pools** (static `+0x2b0`/stride 0x440, dynamic `+0x2c0`/
  stride 0x5b0, counts `+0x498`/`+0x49c`), then pushed into the BlockData geom_ins vector `+0x288`. Three
  add-routes ranked in the findings; **recommended first probe = route 1 `spawn_clone`**: allocate 0x5b0,
  `FUN_1406b9880(mem, srcType, CLONED existing parts rec, transform)` (reuse a resident asset so its model
  is loaded), copy a live sibling's `+0x220` matrix + offset it, push into `+0x288`; the ctor self-registers
  WGM/render/physics. Open sub-Qs (a live probe answers): does a cloned `CSMsbPartsGeom` satisfy the ctor's
  reads; is the `+0x288` push enough for render+collision or is a pool index assumed elsewhere. This is a
  multi-step build, NOT a quick primitive like move.
  **⇒ RESUME (next session): standalone-ctor ADD is a DEAD END (builder decompiled, 726f6189). Pivot to a
  streaming-path spawn. `docs/re/windows_geom_spawn_builder_re_findings.md`.**
  The pose-descriptor builder `thunk_FUN_144cbdae7` is MSVC-EH-wrapped + welded to the tile-streaming
  context; calling it standalone FROZE the game (live-confirmed). `arg4=0` is exactly what the working driver
  passes → the hang is CONTEXTUAL, not an arg bug — decomp (726f6189) confirmed there's no arg fix and no
  cheap independent `param_4` (alias guts the source, copy double-frees the owned sub-objects). So
  hand-driving the Dynamic ctor from a standalone RPC does NOT work. **`spawn_clone` was NEUTRALIZED** — it
  now only does the safe arg recon and returns the DEAD-END string (never calls the builder/ctor; no longer a
  footgun). **Real ADD = pick a pivot (from the findings):** (1) spawn on the streaming thread — hook the
  tile-stream driver `FUN_1406a7930` and inject one extra part into its per-part loop (heaviest, correct);
  (2) the asset-request path `FUN_1406a5080`→`FUN_1406c7000` — **ASSESSED 2026-07-04
  (`docs/re/windows_geom_spawn_pivot2_re_findings.md`): this is THE viable ADD route.** `FUN_1406a5080` is a
  non-blocking asset-request REGISTRAR (name/id → RB-tree at `reqMgr+0x318`, state 4, streamer services it on
  ITS thread), and consumer `FUN_140699670` proves the path yields a tracked, player-positioned placement
  from a NAME. Beats pivot 1: no standalone hang, the engine builds the owned descriptor itself, name-driven.
  **Q1/Q3/Q4 DONE (2026-07-04):** reqMgr singleton = **`[DAT_143d69ba8+0x30]`** (er+0x3d69ba8, FD4Singleton);
  name format = **`"AEG%03u_%03u"`** (request an asset by its AEG###_### name); owning feature = a periodic
  player-proximity raycast AEG-asset STREAMER (`FUN_140699670`/`d80`, steps `FUN_140699170`/`FUN_14069a550`) —
  a ready template proving "name + world pos → streamer spawns+tracks the asset." **Nuance:** the path streams
  from a KNOWN-asset registry → ideal for placing copies of EXISTING AEG assets (the world-editor case);
  arbitrary-new needs tree registration. **Q2 DONE (2026-07-04): pivot 2 STATIC RE is COMPLETE.** The `req`
  object has the EXACT `CSWorldGeom` instance layout, and `FUN_1406c6050(req,4)` is its per-frame
  load/visibility state machine: `FUN_1406c8750`→`FUN_1406a6630` (same block instance-registry the ctor uses)
  + `FUN_1406e38c0` (scene/render node + world matrix `FUN_1409f1320`). So `FUN_1406c7000` allocates a REAL
  geom instance (reconciles the earlier "just a name builder") and a serviced request → a real,
  block-registered, rendered instance — NOT visual-only. Collision follows the standard world-geom path (one
  live checkmark left). **reqMgr singleton AOB FOUND (2026-07-04, pyghidra byte-scan) — pivot-2 STATIC RE is
  100% DONE.** `GEOM_REQ_MGR = "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 16 B8 01 00 00 00 48 8D 53"` (er+0x1dcc53,
  UNIQUE image-wide; `relative_offsets {{3,7}}` → `&DAT_143d69ba8`; `reqMgr = *(singleton+0x30)`; backup AOB
  in the findings). **✅ LIVE PROBE WIRED + RUN 2026-07-04 (`goblin_geom_spawn.{hpp,cpp}` + `spawn_asset`
  RPC):** GEOM_REQ_MGR + backup AOBs `[SIG]` PASS + UNIQUE (er+0x1dcc53 / er+0x1dc930, match commit); reqMgr
  chain resolves in-world (`reqMgr=*(singleton+0x30)`). **BUT the direct `FUN_1406a5080` call DEADLOCKS** —
  our RPC runs on the PRESENT thread (pump()) yet the call froze present (watchdog, no exception; workers
  alive) = lock inversion vs the streamer on the reqMgr RB-tree (`mgr+0x318`). So the static "no hang" claim
  fails for an off-native-thread call. RPC is now safe-by-default (resolve-only; `force` to fire → hangs).
  **⇒ NEXT = call it on the game's MAIN-UPDATE thread** where the proximity streamer (`FUN_140699670`) calls
  it safely: hook a per-frame main-thread step (`FUN_140699170`/`FUN_14069a550`) and inject the request there
  (the heaviest-but-correct pivot-1 shape), NOT a standalone RPC/present-thread call. Full result in
  `docs/re/windows_geom_spawn_pivot2_re_findings.md` item 2. MOVE stays fully solved.
  **Live recon (2026-07-03, `spawn_probe` + `test_spawn_probe.py`, fresh DLL) confirmed srcType + corrected
  the layout:** on a real dynamic instance (`AEG004_903`) — srcType@+0x08 `0x3c1412016ff00000` (geom tag ✓,
  hi==BlockData tag ✓; masks g0=0xff/g1=0x14/g2=0xfffff); **param_3 = the BlockData** (inst+0x10, NOT a
  cloned record — so `rec+0x18b` = BlockData+0x18b = 0, registry on BlockData `+0xe8/+0xf8` cap 1024 room);
  **the transform module is at inst+0x20** (heap ptr), not +0x18; CSMsbPartsGeom sub-object embedded at
  inst+0x30 (vt er+0x2ba6738); the **`+0x288` geom_ins vector is EXACTLY FULL** (n=41). ⇒ spawn_clone passes
  the source BlockData, self-allocs 0x5b0, and can **SKIP the +0x288 push** for a first render probe (ctor
  self-registers into WGM/render; untracked-for-unload leak is fine for a throwaway).
  **Move-init risk SOLVED (9081c7c8, Windows-Ghidra):** `param_4` is a **~0x188-byte pose DESCRIPTOR**
  (not a 24B handle); `FUN_1406c3180` field-swaps ~0x188 bytes AND move-constructs the embedded
  CSMsbPartsGeom at self+0x30 from param_4+0x18 (`FUN_140cef4a0`) + steals the heap pose ptr param_4[1] into
  self+0x20 — so a source-aliased param_4 WOULD gut the source (and a deep-dup is a trap: pointer-rich
  sub-object). **FIX = don't alias, REBUILD:** call the driver's own builder
  `thunk_FUN_144cbdae7(&out, BlockData, partsList=*(BlockData+8+0x48), *(BlockData+8+0x58))` (er+0x6c3910,
  4-arg sig identical at both driver sites) → a fresh OWNED descriptor; the ctor move-inits from OUR copy,
  source untouched. **⚠ SUPERSEDED (726f6189):** this "rebuild `param_4` via `thunk_FUN_144cbdae7`" fix is
  DEAD — that builder is MSVC-EH-wrapped + streaming-welded and **HANGS** called standalone (it's not a
  transform math fn; it does resource/streaming work on the wrong thread). So the whole route-b/standalone-ctor
  recipe below is abandoned; kept only as resolved static facts. Real ADD = pivot 2 (above).
  Blocker 1 (srcTypeDesc) = 8-byte packed FieldIns id, buildable (live cross-check ✓: geom_dump inst+0x08
  has the `0x6…` tag + block-tag high32). Blocker 2 (transform=24B FD4 pose wrapper) SIDESTEPPED via **route
  (b): spawn at the source transform, then `SetWorldMatrix`-move to the offset (reuse the proven move
  primitive).** **`FUN_1406c7000` CHECKED + downgraded (8a0e37a) — NO shortcut:** asset-name/streaming-REQUEST
  builder, not a leaner instance factory → drive the Dynamic ctor `FUN_1406b9880` directly.
  **Blocker 3 SOLVED (4e405a1):** the ctors read ONLY `rec+0x18b` (a char flag) from the parts record — the
  prompt's guessed `rec+0x124/+0x3b/+0x3c/+0xd` are the TRANSFORM module (`param_1[4]`/self+0x20), NOT record
  fields; no model-ref read. The Dynamic ctor also MUTATES the record — `FUN_1406a6630(record, inst)`
  registers the clone into the source record's instance list (`rec+0xe8` slots/`+0xf8` cursor/`+0xfc` cap,
  guarded). Route (b) reusing the source record is field-safe (only `+0x18b` matters); the clone just becomes
  tracked by the source record's lifecycle (fine for a dev probe; production would synth a minimal record).
  **(The above blocker-1/2/3 facts are TRUE but pertain to the abandoned route-b; the Proton `spawn_clone`
  probe was tried and is a dead end — see the ⚠ SUPERSEDED note. NEXT is pivot 2, at the top of this bullet.)**
  **Freecam** (dev tool, after ADD): recon done (`windows_freecam_re_findings.md`), **Route 2** = freeze
  ChrCam + override the render view matrix in `GameRendCameraSet` (er+0x680460). BLOCKED on Ghidra: that
  matrix offset + a `CSCameraImp` singleton AOB. Then I code freeze+override from Linux.
  **Disk 90GB scare (2026-07-03) = NOT our code — closed.** User lost 90GB while ER ran, recovered on close.
  Verified: our logs 27M, no ER core dump, `move_all` (16927 insts) had zero disk effect, disk stable in
  automated tests. The Discord "16GiB deleted" files are `memfd:*_pool_shadow` (Chromium PartitionAlloc, **0
  real disk** — lsof shows logical size only). Leading suspect = **Discord Clips recording ER** (journal
  confirms Clips sessions per ER launch) filling a disk buffer during REAL gameplay; user disabling Clips is
  the fix. To re-confirm if it recurs: watch deleted-open files by REAL `st_blocks` (not logical size), while
  actually playing. Don't re-investigate our code.

- **Dev "creative mode" mini-track — SCOPED 2026-07-03 (do after ADD; not on ADD's critical path).** Two
  small/moderate RE items that together give a dev sandbox loop (warp into a throwaway map + fly around +
  place/move/verify) so world-editing experiments (move/ADD/spawn) run ISOLATED from the live save's world:
  - **(1) Warp-to-(mapid, x,y,z) dev primitive.** Today `warp <graceId>` uses `LuaWarp_01` — warps to an
    existing GRACE/bonfire by id (gated on a grace at the target). Missing = a coordinate/map-id warp (ER's
    `WarpToMapPoint`-style debug warp) so the dev can drop into ANY existing map at any coord. Moderate RE
    on top of the warp infra (`LUA_WARP` sig, `CSLuaEventManager`) + player-pos/mapid slots already RE'd.
    Sandbox target = an existing SPARSE/empty map (arena/coliseum, a cleared legacy dungeon, or an overworld
    tile) — do NOT need to create a new map (that's the capstone below).
  - **(2) Freecam.** Not tracked anywhere yet (new). Detach the camera from the player + drive it via input
    — the natural verification+authoring tool for move/ADD + the World Editor (the move/ADD confirms all
    need eyeballing the world at an arbitrary position). Moderate, well-trodden ER RE (FD4/debug camera
    struct + detach flag + input), NOT a frontier. Prompt to write: `docs/re/windows_freecam_re_prompt.md`.
  - Combined payoff: warp (1) + freecam (2) = a "creative mode" dev loop; each is cheap alone, big together.
  - **NOT this track:** creating a genuinely NEW empty map/page FROM SCRATCH = full map creation (new MSB +
    collision + streaming/worldmap registration) — strictly harder than ADD, the map-content **capstone**.
    Explicitly LATER; the sandbox uses an existing map instead so it isn't blocked on this.

- **Live marker regeneration (real-time map editing) — v1 DONE 2026-07-03; v2 open.** Markers build once
  at boot; to reflect a LIVE param edit on the DRAWN map without a game reload, **`refresh_markers` RPC**
  (→ `overlay_api::rebuild_markers` → `worldmap::rebuild_markers`, the production toggle-rebuild path) now
  forces a fresh bucket build. Verified: after a `pickUpItemLotParamId` repoint, `refresh_markers` ran a
  full `build.buckets` (2381 ms) on the detached disk WORKER thread (no frame freeze), re-reading live
  params; game alive. Since the rebuild uses the same live resolve as `loot_at`, existing-lot edits
  (repoint, `lotItemId01`, any param override) now show on the map.
  **v2 (b) DONE 2026-07-03 — cloned lots now resolve.** The 5 `LotReader` caches in
  `goblin_loot_resolve.cpp` were consolidated into ONE shared, mutex-guarded reader (callers copy the
  0x98-byte row out under the lock, then read lock-free) with a public `goblin::reset_lot_reader()`;
  `rebuild_markers()` calls it synchronously before kicking the worker, so a lot CLONED live
  (`param_clone`, which reallocates `param_header->param_table` — the pointer the reader snapshots at
  construction) is re-read on refresh. E2E-proven (`test_world_editor.py`, 16/16): a cloned lot reads
  `textid=-1` (invisible) BEFORE refresh and resolves its item AFTER.
  **v2 (a) DONE 2026-07-03 — parse cached, refresh ~60% faster.** Measured: `build.buckets` was
  ~3160ms of which the MSB parse (`load_disk_treasures`, ~480k asset placements) is ~1820ms. That parse
  output doesn't change on a PARAM edit (only the live per-marker resolve does), so it's now cached in a
  file-scope `ParsedDisk` (keyed by the source "want" flags; MSB files are immutable for the process, so
  the key is the only invalidation). A param-only `refresh_markers` reuses it (`[BENCH] build.disk_parse:
  CACHED`) → **build.buckets 3163 ms → 1262 ms**. The two vectors the build augments in place
  (`disk_collectibles` LOD-feature append, `disk_enemies` LOD-award append) get a cheap working copy;
  the rest are read-only refs into the cache (const-checked by the compiler). New `[BENCH] build.disk_parse`
  line isolates the parse cost. E2E still 18/18 (markers unchanged). Remaining perf idea (not needed):
  truly INCREMENTAL per-bucket regen — the parse cache already removes the dominant cost. NB the copy is
  ~30MB resident; acceptable. Gate any AUTO-trigger vs the collected-graying contract + `read_wgm` spike.
- **F1 panel to edit param overrides live** — optional polish on the param-override framework (all 3
  loader slices are done/merged); more registry fields = one AOB each. Not started.
- **Gap C GRANT — grant+sidecar PROVEN 2026-07-03; NAME + author surface remain.** A CLONED custom
  goods row grants into inventory and is kept out of the vanilla `.sl2` (`test_gapc_grant.py` 4/4 +
  boot-2 clean 1/1). Two findings baked into `custom_item_end_to_end_plan.md`: (1) **grantable goods-id
  ceiling `0x7FFFFE`** — `give_item` no-ops at ≥`0x7FFFFF`, so the old reserved band `90000001` was
  never grantable; use ≤`0x7FFFFE` (the test uses `8000000`). (2) **`fmg_set` slot: base `10`
  WORKS, DLC-tier `419` FREEZES** the present thread (RPC marshals there). Inject names at slot 10;
  the 419 hang + which slot the item-name UI reads are handed to a Windows/Ghidra sweep
  (`docs/re/windows_fmg_slot_re_prompt.md`). **RESOLVED (static, 2026-07-03,
  `docs/re/windows_fmg_slot_re_findings.md`):** goods-name UI (`FUN_140d10680`) reads
  `menu(111)→base(10)→dlc01(319)→dlc02(419)`, so a NEW id renders at base **slot 10** (111/DLC empty
  for it); the 419 freeze is our `patch_fmg_in_memory` doing a `fileSize − str_data_start` size_t
  **underflow** on a DLC-stub header (NOT the group loop — hence the reverted span guard didn't help)
  → multi-GB resize/memcpy on the present thread. Fix = O(1) offset/size sanity guard + reject slots
  ≥300 and the 11x menu tier; keep injecting at base 10. **DLL guards CODED + verified 2026-07-03**
  (`goblin_messages.cpp`: `patch_fmg_in_memory` offset/size + span-vs-stringCount guards;
  `inject_fmg_entries` slot policy): `fmg_set 419` now returns a fast error (game alive), `fmg_set 10`
  works, boot PlaceName(19)/TutorialBody(208) injects unaffected.
  **✅ Author surface DONE 2026-07-03** — `custom_items.toml` (TOML chosen over JSON for hand-authoring;
  toml++ header-only). `goblin_custom_items.{hpp,cpp}` applies each `[[goods]]/[[weapon]]/…` at boot
  (clone+fields+name) + `sidecar::register_author_item` (declarative registry: granted on world-enter,
  stripped pre-save, NEVER in the `.mfg` — re-applied every boot). E2E `test_author_items.py` 1/1: toml
  → boot → world-enter grant → `goods_count==qty`. Example `custom_items.example.toml`.
  **Remaining polish only:** finalize the reserved band from a param-scan survey; `decode_textid`
  read-back chain parity (menu-first `{111,10,319,419}`); more categories as needed. **Gap C is
  functionally complete.**
- **MapGenie coverage — Hidden Passage category, not started.** Hit-detected illusory walls, no action
  button → no static signal to parse (hardest remaining Group-2 category). RE notes:
  `docs/re/windows_group2_landscape_re_findings.md`.
- **MapGenie coverage — Wandering Mausoleum, not attempted.** Dynamic moving entity, no static MSB
  signal; low priority.
- **RPC auto-idle (`feat/rpc-auto-idle`) — needs in-game verify.** Built + deployed
  (`src/input/input_wndproc.cpp`, `goblin_debug_rpc.cpp`, ini `[Debug] rpc_auto_idle` default true):
  scripted RPC input (`key`/`mouse_*`) should self-suspend for ~1.5s when the human touches real
  keyboard/mouse, and NOT self-idle from its own injected input. Verify with the map open: wiggle the
  real mouse → `status` shows `rpc_input_idle=1` within ~1.5s and an RPC `key` is refused; stop
  touching input → resumes after ~1.5s; a scripted `type`/`key` run must NOT trigger it. Dev-only
  tooling, no changelog line on pass — just merge. Detail: `docs/memory/tooling/mfg-rpc-driver-hardening.md`.
- **Silent deadlock freeze — UNSOLVED.** One occurrence (2026-07-02): log goes silent (no crash, no
  exception), window solid, RPC thread alive. Distinct from the known `eldenring.exe +0x1EB9999` exit
  crash (that one's handled: TerminateProcess after triage). Shipped the catcher —
  **freeze watchdog** (`goblin_freeze_watchdog.cpp`, ini `[Debug] freeze_watchdog_secs`, default 20s):
  present-thread heartbeat; on stall writes `logs/MapForGoblins_freeze_<pid>.txt` + a full-thread
  minidump. **Next freeze → symbolize the dump with the deployed PDB and root-cause.**
- **Background-focus RPC driving — partially closed.** Root cause found: our own `g_has_focus` gate
  kills keyboard poll + mouse clicks off-focus (not the pause system). The first `key` after
  auto-refocus being silently lost is fixed (closed-loop retry via `hk_wndproc` arrival counter). Still
  open: `mouse_click`/`type` have no delivery-verify (same loss window), and "drive UI while the user
  works elsewhere" needs a dev-mode treat-as-focused override (accepted tradeoff: RPC keystrokes leak
  into the backgrounded game, symmetric with how PauseTheGame's global hotkeys already behave). Not
  started; until then keep the game window focused during scripted UI runs.
- **F2 fog-locate pan clamp — reverted fix, real bug still open.** Locating a target in undiscovered/
  fogged territory (e.g. Morgott while Leyndell is fogged) clamps the pan at the edge of revealed area
  instead of centering the target — deterministic repro documented. A fix attempt (direct pan/snap-rect
  writes, zoom-easer write) was REVERTED by user call; **read
  `docs/re/linux_f2_fog_locate_clamp_re_findings.md` before retrying** — the real blocker is the engine
  clamping the cursor reticle inside a `c32f0` step whose bounds source isn't in any struct we've found
  (needs Ghidra on the `c32f0` subtree). Hard constraint for any retry: non-fog locates must behave
  exactly as today, no per-frame write fights, no forced zoom.
- **Baked-data → runtime/disk migration — IN PROGRESS.** Authoritative plan:
  `docs/plans/baked_data_full_removal_plan.md` (6 phases; `build_pipeline.py` deletion is Phase 5, the
  END state, not the first step — it still generates tables with no runtime source). Landed: Phase 1
  (enemy-drop labels), name-alias English search (now reads live `msg/engus` off disk), several
  category-exception bakes recovered live via `EquipParamGoods.sortId`. **Next pick (easiest→hardest
  per the plan's inventory):** dedup `goblin_tile_tabs`/`goblin_major_regions` (identical across
  profiles now that there's only one profile — pure housekeeping); assess
  `goblin_region_anchors`/`goblin_name_regions` vs `WorldMapPointParam`+`WorldMapPlaceName`; the icon
  atlas (biggest remaining item, see next bullet). Minor unblocking follow-up noted, not gating: a
  handful of Reforged item families / DLC key items still fall into the "Loot - Crafting Materials"
  catch-all on colliding sortIds — needs dedicated rules or accept the catch-all.
- **Baked-atlas removal — DEFERRED, gate not passed.** `[ICONTIER]` census (kept in-tree for
  re-auditing) shows ~15 categories still resolve only through the baked atlas on ERR and/or vanilla
  (Hostile NPC, Spirit Springs, Stakes, Cookbooks, Crystal Tears, Golden Runes Low, …); until native/
  disk resolution covers those, the atlas stays. Re-run recipe and follow-up ideas in the file this
  replaced (`git log -p -- docs/HANDOFF.md` if needed) or re-derive via the `[ICONTIER]` census tool in
  `map_renderer.cpp`.
- **Lag-spike hunt — `read_wgm` cache-miss path still spikes.** The steady-state RB-tree walk was fixed
  (bulk RPMs, `read_rb` helper) and AVG dropped to ~0.05ms, but fresh-tile loads still spike 2-3ms
  (~33x) because every new tile re-reads each geom instance's full chain (~3 RPMs/instance) before the
  AEG family filter drops the noise. Next ideas: budget cache-miss resolution per refresh (check the
  collected-graying contract first so a deferred tile doesn't flash wrong), or land an AOB-pinned
  O(1) collected getter to skip the RPM snapshot entirely (`goblin_collected.cpp:543` already has a
  DR0 armed for this). NB `present.overlay_total`/`present.newframe` spikes were investigated and
  RESOLVED AS WONTFIX — game-side frame cost and a one-time ImGui font-atlas upload, not our code.
- **Map-exit input softlock — external cause, low priority.** Root cause is Deskflow (cursor-sharing
  KVM), not this mod or ER; fix is Deskflow-side. `docs/re/windows_input_softlock_re_prompt.md`.
- **Open policy question: is non-ERR/vanilla a hard support target?** Decides whether ERR-leaning bakes
  (atlas, etc.) can eventually be dropped entirely or must stay as a permanent vanilla-compat net.
- **Double-DLL-load hardening — not implemented.** Strategic fix (single-DLL migration) landed and
  prevents NEW installs from double-loading, but an existing install with a stale `_vanilla.dll` still
  can. TODO: a named-mutex check (`CreateMutexW`) at init so a second instance bails before installing
  any hooks and shows an on-screen "double load detected" banner instead of silently double-drawing.
- **Clang-only toolchain — Phase 1 mostly done, matrix open.** `build.bat` (ninja+clang-cl) and
  `build.bat snapshot` are both validated on Windows (packaging + PDB archival proven). Still open:
  `build.bat release` (version-bump path) unexercised; Phase 2's real in-game validation matrix.
  `docs/plans/clang_only_toolchain_plan.md`.
- **Big-files refactor — items 1+2 done, 3-7 open.** `docs/plans/big_files_refactor_plan.md`: done =
  panel split into `src/overlay_panel/`, shared marker gates. Remaining: classify dedup, diag
  quarantine, `icon_uv`, god-function breakup, grace-sprite design.
- **Real map clipping (RE the game's own clip) — not started.** Would replace the exclusion-zone
  stopgap (dial disc + user rects) with pixel-perfect clipping identical to the game's own map/minimap
  clip. Big RE; low priority, current stopgap works.
- **Zoom+pan simultaneous 1-frame icon "dash"** — stale projections streak icons for a frame when zoom
  and pan change together. Suspect: the ViewDelay ring interpolating pan/zoom inconsistently. Not
  investigated.
- **Fan (spiderfy) near a screen edge can overflow off-screen** — the canvas clip trims it but doesn't
  re-anchor the fan. Minor, not investigated.

### Decided against (don't re-propose without reading why)

- **Merchant map pins (search Slice 3) — SHELVED 2026-07-03 after an RE spike.** The shop↔NPC join is
  talk-ESD-only (confirmed no EMEVD `OpenRegularShop` signal exists); pulling a shop-id range out of ESD
  needs a full EzState bytecode evaluator — disproportionate for one pin category. Merchant item
  *search* (Slice 1) is the shipped feature; naming the seller (Slice 2) was separately deferred for
  the same ESD reason. `docs/plans/merchant_item_search_plan.md` Slice 3.
- **F2 zoom-easer write fix — REVERTED.** Mechanically worked but forced an uninvited zoom on every
  fog-locate plus a visible flicker fight on clamped targets; user rejected the UX tradeoff. See the F2
  entry above for the real fix direction.

## Standing gotchas & non-obvious facts

- **RPC/driver scripting gotchas** (full detail `docs/memory/tooling/mfg-rpc-driver-hardening.md`): a
  background job can only keep ER alive via a single FOREGROUND blocking bash command (me3 as an
  in-shell child, killed before return); `ping` ≠ game alive, gate on real liveness; AZERTY layout means
  SendInput's VK→scancode uses US but the return scan→VK translation uses the HOST layout, so scripted
  `type` must send QWERTY-position characters, not the intended letters; `mouse_move` needs the
  SetCursorPos trampoline + a real ±1px jiggle event (absolute SendInput alone lands off-target and the
  game re-warps the raw-input reticle onto the old position after one frame) — send it twice, a rare
  warp race eats the first; `pkill -f "Game/eldenring.exe"` also matches the driver shell's own args and
  kills it early.
- **Wineserver RPM contention:** many small `ReadProcessMemory` calls in a hot per-frame path can
  contend with the render thread even at sub-ms each, because wineserver serializes ALL RPM calls
  process-wide under Wine — batch reads into as few RPM calls as possible (lesson from the `read_wgm`
  spike fix).
- **Double-DLL-load is not a code bug.** If both an ERR and vanilla DLL variant ever end up in the mods
  folder, both load into the same process → doubled ImGui draw, doubled PlaceName patch, etc. Single-DLL
  migration prevents this for new installs; see the hardening TODO above for stale existing installs.
- **AOB doctrine:** pin code sigs, never raw RVAs — the WorldChrMan resolver was flipped from
  RVA-first to AOB-first after an audit found it violating this (a future ER patch that moves the slot
  would otherwise silently go stale). `goblin_world_position.cpp`.
- **Grace icon scale is deliberately SEPARATE from the generic marker scale** — calibrated for vanilla
  parity when the cursor locks onto a grace; do not fold it into the shared scale knob.
- **Golden-rune glow sizing:** size any glow/backing effect off the icon's NATURAL draw size (`base_hh`,
  a ratio), not the post-bump scaled size — sizing off the scaled size produced a big dim wash instead
  of a compact bright orb.
- **The 7 mod-added POI categories** (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have no ERR-custom glyph; their massedit iconIds
  (374+) point at glyphs absent from every current menu file (numeric glyphs cap at 261) — recover via
  a real `SB_MapCursor` glyph where one visually fits, else circle.
- `MENU_MAP_ERR_*` (boss/grace) names are ERR-only; they won't resolve off-ERR → falls back to circle
  if the baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
- i18n: `overlay_language = auto` reads the WINE prefix locale under Proton (usually `en_US` even on a
  French desktop) — French users should set it explicitly. Avoid `œ` in translations (outside the
  merged font ranges, use "oe"). Keep label translations ≲ English+20% (panel caps at 840px).
