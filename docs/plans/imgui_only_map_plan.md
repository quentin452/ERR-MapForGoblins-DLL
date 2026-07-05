# Master plan — ImGui-only map (retire the native ER map)

Status: **DESIGN 2026-07-04, user-reviewed — decisions locked (see below).** This is the TOP-LEVEL
sequencing plan. It doesn't restate the mechanism sub-plans — it orders them and adds the two things
that were missing: a hard **parity gate** and the **grace warp menu** feature.

## Locked decisions (user review 2026-07-04)
- **THIS is the PRIORITY track (confirmed 2026-07-04).** The ImGui-only map (Track 1) comes before the
  runtime-modding RE (Track 2 = dev-dimension/teleport, MSB-write, ESD, weapon arts). See the **Roadmap**
  section below — the old track order (C0→A→B→C1→D2→C2/C3→polish) is now framed as milestones **M1→M5**.
- **Grace menu home (Track B):** a **collapsible SIDEBAR on the vmap** (list + canvas in one surface).
- **Relief base (Track D):** **D2 raycast heightfield** (mod-agnostic) as base; D1 ART optional overlay.
- **Map key (Track C2): RESOLVED 2026-07-04 — no hardcoded key needed.** The vmap already opens on
  `goblin::world_map_open()` (the game's REAL menu state, `CSMenuMan+0xCD==7`), so it reacts to the game's
  map actually opening — **remap- AND gamepad-agnostic for OPEN, for free** (whatever the user bound). The
  `,`/`m` worry is moot. What DOES need ER's keybind system: gamepad **navigation** of the vmap + remap-aware
  action triggers → `docs/re/windows_keybinding_config_re_prompt.md` (`CSPcKeyConfig = DAT_143d5deb8`
  identified; gamepad-nav needs NO decode — feed the polled XInput into ImGui nav, see M1/M4).

## ⭐ Roadmap (confirmed 2026-07-04) — Track 1 (this plan) is the PRIORITY

Two independent tracks; the user confirmed **Track 1 first**:
- **Track 1 = the ImGui-only map (THIS plan).** It BUNDLES the map ImGui work AND the RE that gates it — do
  that RE *as the migration needs it*, not as a separate later phase (there is no "migrate, THEN RE": the
  migration is gated on the cull + gamepad RE).
- **Track 2 = runtime-modding RE** (dev-dimension/teleport, MSB-write, ESD/EzState, weapon-arts) — ORTHOGONAL,
  the follow-on. Tracked in `docs/HANDOFF.md` + per-topic prompts; NOT worked until Track 1 ships.

**Track 1 milestones — M3 is the SHIP line; M4/M5 are polish:**

| M | Milestone | Maps to | RE it needs |
|---|---|---|---|
| M1 | **Cheap wins, zero new RE** — ✅ CODE DONE 2026-07-06 (needs live verify): (a) **gamepad nav already wired** — `NavEnableGamepad` set + `ImGui_ImplWin32_UpdateGamepads` polls + the XInput hook's `caller_is_us` check feeds ImGui real pad while zeroing the game's (input_gamepad.cpp); no code needed, verify in-game. (b) **heightfield cast window WIDENED** ±2000→(+3000/−10000) via `kCastAbove`/`kCastDepth` (goblin_heightfield.cpp) — fewer within-block misses. (c) **sea-tag PLUMBED** — `Cell.sea` + classify hits below `kSeaLevelY` + water-blue render branch (panel_virtual_map.cpp); DORMANT (sentinel -1e30) until a live shoreline `hf_probe` calibrates the sea Y (then flip 1 const). | none |
| M2 | **Content parity** | Track A rows + D2 heightfield (UNBLOCKED — present-thread cast works, `d3ca993`) + tiles; far-terrain OPTIONAL for full-map coverage | `far_terrain_heightmap_re_prompt.md` (optional) |
| M3 | ✅ **USABLE — "our map shows"** | `world_map_open()` → vmap **cover opaque** (accept the native draw cost for now), mouse+kb | none (open is state-based → already remap/pad-agnostic) |
| M4 | **Nav parity** | warp-on-click (done), close, region switch, **gamepad nav** | `windows_keybinding_config_re_prompt.md` (remap-aware triggers); gamepad-nav itself = M1, no decode |

**M4 gamepad live-findings (2026-07-06, user-tested M1 build):** ImGui widget-nav works, but the vmap CANVAS needs custom pad handling (ImGui nav only moves widget focus). Landed a first pass (render-side, `panel_virtual_map.cpp`, hot-reloadable): **LEFT stick pans, L2/R2 zoom about a RIGHT-stick virtual cursor (reticle drawn on top), pad-mode latch (exits on real mouse move).** RIGHT-stick/triggers chosen so they don't fight ImGui nav's LEFT-stick/dpad. **STILL OPEN (queued):** (1) **on-screen keyboard** for the item-search InputText — ImGui has none; gamepad can't type → search unusable by pad. Options: build a tiny ImGui OSK grid driven by pad, OR hook ER's native software keyboard. BIGGEST piece. (2) **pad cursor → hover/click/place** — the reticle is visual-only so far; feed it as the canvas pointer so pad can hover item tooltips + place/delete custom markers (needs button binds that don't clash with nav Activate). (3) tune stick speeds/sign live. (4) map-OPEN by pad/mouse = M3 (vmap must replace ER's map first), not a bug.
| M5 | **Cull native (perf)** — ⚠ both CHEAP levers DEAD (2026-07-05), DEPRIORITIZED | suppress the native DRAW (cover ≠ cull). D3D12 scissor + GFx `MovieImpl+0xB0` clip write BOTH disproven live (`..._render_toggle_re_findings.md` §4c/§4d). Left = Scaleform draw-vfunc no-op / movie visible-flag / CSMenuMan draw-skip = **Windows Ghidra, not-cheap**; gated on parity+fast-travel anyway | Windows RE (draw-vfunc); `CANVAS_SINGLETON` for the tile canvas |

**Key ordering insight:** M3 (usable, cover-opaque) ships BEFORE the cull (M5) and gamepad (M4). Cull +
gamepad are quality/perf upgrades, **not blockers** for "our map replaces theirs visually" — so Track 1
reaches a shippable milestone early, then polishes. The **PARITY GATE** (Track A, below) still governs M2→M3.

Sub-plans it sequences (already scoped):
- `single_surface_ui_plan.md` — the native-map takeover mechanism + map-key bind + slices 0–3.
- `virtual_world_multi_world_design.md` — mod-owned worlds, open-via-M (slice D done).
- `map_tile_loading_plan.md` — real ER map ART on the canvas (slices 1a/1b/2/3a done).
- `procedural_map_derivation_design.md` — Convergence-trap-safe object/terrain derivation.
- RE: `windows_terrain_raycast_heightfield_re_findings.md` (relief), `windows_loading_screen_state_re_findings.md` (warp freeze).

## North star (why ImGui-only fixes everything)

Make the **Virtual World the ONLY map surface**; the native ER map never renders. Going ImGui-only
kills whole bug CLASSES for free, not just simplifies code:
- **Fog-of-war bug class** — the native map's map-fragment reveal gating. Our canvas has no fog; it
  shows everything the mod knows. Gone.
- **1-frame latency** — the "draw our markers ON the game's map" path reads last frame's view and lags
  a frame. An owned canvas draws markers in the same frame → no motion desync (the baked frame-delay
  hack in `map_renderer` becomes unnecessary).
- **Duplicated if/else** — one draw path, not "native overlay" + "vmap" (partly done, `563a00e`).
- **The vmap warp freeze** — likely the native map menu still open underneath (see single_surface plan;
  the load watchdog `cb2eb0b` will confirm from the stall dump).

## ⛔ The hard rule: PARITY GATE before the switch

Do **not** wire the map key to open the vmap-instead-of-native until the vmap has PARITY with
everything the native ER map + the F1 worldmap features do today. Flipping early = regressions the
user notices. Track A is the gate; C's final step (bind the key / disable native) is blocked on it.

---

## Track A — PARITY CHECKLIST (the gate) — REVIEW EACH ROW

Every native-ER-map / F1-worldmap capability, and whether the vmap already has it. `?` = needs an
audit pass to confirm. Fill/confirm this table first; each unchecked row is a blocker.

**AUDIT DONE 2026-07-04** (read-only pass): 6 of 7 audited rows MISSING, 1 PARTIAL. The vmap is far
from parity — Track A is real BUILD work, not just a checklist. This IS the go/no-go list.

| # | Capability (native map / F1) | vmap today | Blocker? |
|---|------------------------------|-----------|----------|
| A1 | Pan / zoom | ✅ canvas | no |
| A2 | Real map ART tiles (overworld) | ✅ slice 2/3a (M00) | partial: seamless full-map streaming = map_tile slice 3 |
| A3 | Underground (M01) + DLC (M10/M11) tiles | ⚠️ **BLOCKED on a fresh RE pass (2026-07-05)** — `virtual_map_load_lod(dim,…)` is dim-generic + RPC-reachable, but placement is offset-broken (name-grid≠runtime grid). The findings' "read LIVE rects" fix (`harvest_resident_tiles`) was driven live and DOESN'T yield tiles: `area+0x390` is a vector of INLINE 0x110 layers (confirmed), but the `+0x230` tile-tree offset is wrong for a 0x110 object AND the active layer's map is EMPTY in the driven state (tiles not resident). See `windows_worldmap_tile_rect_reach_re_findings.md` §7. Next: Ghidra `FUN_1409da900` for the real tree offset + find what makes tiles resident; OR solve archive-name↔cell (deferred texture brief). `vmap tile_recon` correlation RPC is ready for when harvest works. | **yes (RE)** |
| A4 | All marker categories (graces/sites/bosses/loot/…) | ✅ reuses native draw (`draw_marker_glyph`) | no |
| A5 | State-aware icons (discovered/collected/cleared/rune glow) | ✅ `563a00e` | no |
| A6 | Marker tooltips (name + count) | ✅ | no |
| A7 | Region name labels | ✅ **DONE** — `panel_virtual_map.cpp` region-label block (`marker_world_pos`→`w2s`, group-gated, `Labels` toggle) | no |
| A8 | Clustering / pile "×N" | ✅ **DONE (`177c73c`)** — quadtree LOD clustering + viewport cull (`marker_quadtree.hpp`); zoomed-out draws "×N" piles, not 6837 raw. Also fixed the perf bottleneck (vmap.markers 4.08→0.51 ms, ~8×) | no |
| A9 | Item search + "locate" pan | ✅ **DONE 2026-07-05** — TWO paths: (1) the F1 item-search result click also locates onto the vmap (`virtual_map_locate`: centroid of the hit markers + switch page); (2) the vmap has its OWN **Item search sidebar** (`Items` toolbar toggle) — token-match over placed markers, deduped rows per (name, page) with counts, click = centre the canvas on it. So item search works fully ON the vmap, native map closed. Screenshot-verified ("grace" → 16 rows: Grace Mimic ×6, Iris of Grace ×2 DLC, …). RPCs `vmap locate` / `vmap items`. Ring-on-vmap = follow-up. | no |
| A10 | Fast-travel to grace (double-click) | ✅ but FREEZES — Track C0 | **yes** |
| A11 | Player position marker + heading | ✅ **DONE `69188c3`** — dot/heading-arrow on the vmap (base ER + matching group), minimap yaw convention through the vmap axis signs | no |
| A12 | ERR day/night dial (ERR-only) | ❌ MISSING — native-map overlay only (`map_renderer.cpp:1170`) | ERR-only, low prio |
| A13 | Map cursor → world readout (dev) | ✅ probe | no |
| A14 | UI exclusion zones | native-map overlay only → **moot** after switch | drop |
| A15 | Legacy-dungeon / sub-area maps | ✅ **CLOSED as parity 2026-07-05** — vanilla ER has NO in-dungeon detailed map for legacy dungeons; the native map ALSO folds them onto the overworld as location points. vmap folds via `WorldMapLegacyConvParam` (`legacy_fold.cpp`) AND draws the dungeon's interior markers at the fold point — so it MEETS/beats native parity. Separate per-dungeon PAGES would be a mod feature BEYOND native, and doing it properly IS the dimension-registry pivot (`virtual_world_multi_world_design.md` L172-214: groups→mapId dimensions) + dungeon sub-map tile art (blocked on A3-tiles RE), NOT a parity blocker. So A15 is not a gate row. | no (parity met) |

**A-BUILD list (the gate) — ordered cheapest/most-critical first:**
1. ~~**A11 player marker + heading**~~ ✅ DONE `69188c3`.
2. ~~**A7 region labels**~~ ✅ DONE — `panel_virtual_map.cpp`: per-anchor `MAJOR_REGION_ANCHORS` →
   `marker_world_pos`(conv_underground) → `w2s`, gated on `active_world==0 && group==s_group`, gold
   text + shadow + pill (same aesthetic as native `draw_region_labels`), `Labels` checkbox toggle.
   Non-interactive (the native click-to-hide chip is native-only). Rides the SAME proven marker
   transform, so it co-locates with its region's markers by construction. Builds clean.
3. ~~**A8 clustering/piles**~~ ✅ DONE (`177c73c`) — a vmap-local quadtree (viewport cull + LOD piles).
4. **A9 item search/locate** (make the F1 search ring + pan target the vmap too).
5. **A3 tiles underground/DLC + placement fix** (map_tile slice 3; the offset gap).
6. A12 dial (ERR-only, low). A15 legacy-dungeon sub-maps = ✅ CLOSED as parity (native folds dungeons too;
   real per-dungeon pages = the dimension-registry pivot, a mod feature beyond native, not a gate row).

**vmap UX backlog (from live testing 2026-07-04):**
- ✅ grace z-order (draw on top) `dd64d8d`; ✅ hover z-order (grace wins the tooltip/warp) `89d0cd8`;
  ✅ focus-player-on-open `cd7948b`; ✅ grace warp id fix (bonfireEntityId) `89d0cd8`.
- **Spiderify — ✅ DONE 2026-07-05.** Ported the native hover-fan to the vmap, covering BOTH the quadtree
  piles (hover a cluster → members fan in a ring ≤12 / spiral beyond, legs back, dedup identical → ×N badge,
  "+N" overflow past 40) AND the residual EXACT/near-coincident SINGLES the unbounded zoom can't separate
  (several items on one loot spot — bucketed by an icon-sized screen cell). Each fanned icon feeds the shared
  hover accumulator so the existing tooltip + grace double-click-warp fire for it. `MarkerQuadtree::Pile`
  gained a node index + `gather_pile()` (on-demand, only the hovered pile pays). Gated on
  `config::clusterSpiderfy`. Screenshot-verified via `vmap spiderfy 1` (force-open the largest pile): the
  spiral fan + ×N/×27/+42 badges render. (Native map has its own spiderfy; this is the vmap-native one.)
- **Perf follow-ups (the big bottleneck is GONE — `177c73c`, vmap.markers 4.08→0.51 ms).** Minor, for
  later, none blocking: (1) QT rebuild is a ~17 ms one-time spike on group change — build incrementally or
  off-thread only if it's ever felt; (2) ✅ **FIXED 2026-07-05** — `region_gated` markers are now excluded at
  QT BUILD time and a region-enabled fingerprint (`s_qt_region_mask`) is folded into the rebuild key, so a
  region-name toggle rebuilds the index → PILES de-count hidden regions too (was: singles hid but piles still
  counted them); (3) cache each visible single's draw recipe (icon UV +
  state) instead of recomputing `draw_marker_glyph` per frame — small now that only the visible subset
  draws; (4) relief draws per-cell quads (`vmap.relief`) — a single texture would scale better at big
  grids; (5) the grace sidebar reads `read_event_flag` per grace per frame (~438) — throttle if needed.
- **"Hors map" stray icons = a PRE-EXISTING DATA bug, not a vmap bug.** A handful of markers carry bad
  world positions (origin (0,0) defaults + garbage like (110767,−59445)) from a failed projection. Those
  were ALWAYS wrong — invisible before only because the native map can't zoom out far enough to show them
  and the vmap's default view didn't frame them. Our unbounded dezoom + Fit made them VISIBLE. The quadtree
  plausibility gate (±40000 box + (0,0)) hides them from the vmap so they stop distorting Fit, but the ROOT
  cause (why those markers project to garbage) is a separate marker-data investigation, unchanged by this.

**A-SETTINGS audit DONE 2026-07-04 → full per-key table in
[f1_settings_imgui_only_classification.md](f1_settings_imgui_only_classification.md)** (~19 DROP, 1
HARDCODE-TRUE `grace_overlay`, ~15 PORT — clustering block dominates; 2 `?` to verify: `native_item_icons`
residency, `dump_icon_textures`/`dump_converters` vmap dependency). Original note below.

**A-SETTINGS audit (user, 2026-07-04): reframe/remove native-map-only F1 knobs.** Many F1 settings
exist ONLY to control the native ER map overlay; once the vmap is the sole surface they're dead or
must be re-pointed at the vmap. Audit every F1/ini knob through the ImGui-only lens (this EXTENDS the
existing `settings_sweep_plan.md` classification — do it there, tagged "native-map-only"). Known
native-map-only → retire after the switch: `graceSuppressNative`, `landmarkSuppressNative`,
`suppressNativeBosses`, `clipGameUi`, `uiExclusionRects` (A14), the ERR `dial*` (A12 unless ported).
Known to re-point at the vmap: `showRegionLabels` (A7), `stackIdenticalItems`/clustering (A8), item
search (A9), `showMinimap` gate (flip to `virtual_map_open()`). Do this as part of Track C3 (collapse),
NOT before the switch — the knobs still drive the native map until then.

---

## Track B — Grace warp menu — ✅ v1 DONE 2026-07-04 (`586a148`)

Collapsible grace-list **sidebar on the vmap** (the locked home): `Graces` toolbar toggle → a searchable,
name-sorted list of all graces (438 on the dev save) with a per-row state dot (gold=discovered via
`read_event_flag`, grey=not). Discovered → double-click TELEPORTS (`s_warp_pending`); any row →
single-click LOCATES (pans canvas + switches group); undiscovered = locate-only. Reuses the canvas grace
layer (`row_id`/`discover_flag`/`name_id`) — no new RE. Screenshot-verified.
**✅ Follow-up DONE 2026-07-04 (`3b49ccd`): region grouping + filter tabs.** Each grace groups under its
nearest same-group major-region anchor (A7 `MAJOR_REGION_ANCHORS`) as a collapsing header (sorted
region-then-name); tabs = **All / Discovered / Undiscovered** (the useful warp-menu filter — the
All/Current/Other-worlds split is redundant with the vmap World selector and only Base ER has graces).
**Remaining nice-to-haves:** flash the located dot on click; per-region discovered count badge.

Original spec (a grace LIST menu, not just clicking dots on the canvas), so the player browses and travels:
- **Tabs:** `All` · `Current world` · `Other worlds` (per `goblin::vworld`; multi-world aware).
- **Rows:** every grace, DISCOVERED and UNDISCOVERED, grouped by region, with the state icon + name.
- **Action per row:**
  - **Discovered** → **teleport** the player there (`warp::to_grace`, the fixed post-frame path).
  - **Undiscovered** → **pan** the vmap camera to its position (no warp — that's the infinite-load
    bug; already gated `0d23028`). Optionally flash the dot.
- **Data source:** the grace layer already carries `{rowId, discoverFlag, textId}` per grace
  (`grace_layer.cpp`); the discovered test is `read_event_flag(discoverFlag)`. No new RE.
- **Home (REVIEW):** a panel on the vmap (sidebar), OR an F1 tab, OR a toggle button on the canvas.
- Kills a native-map-navigation pain point AND showcases the ImGui-only advantage (search/sort/filter
  a grace list — impossible on the native map).

---

## Track C — Native-map takeover + map-key bind (→ single_surface_ui_plan.md)

- **C0 RESOLVED (2026-07-04) — the warp freeze was NOT menu-context and NOT DLC-destination; it was
  two vmap DATA bugs.** The watchdog stall (`load_stall_332`, area 61) was a red herring caused by a bad
  warp id. Root causes, both fixed: (1) the grace marker's warp id used the BonfireWarpParam ROW KEY
  (ERR remaps it, 61423601) instead of `bonfireEntityId` (1042362951) → `89d0cd8`; (2) the warp OFFSET
  was the CT's `-1000`, which lands one bonfire off — correct is `0` (entity id direct) → `088aabc`.
  Grace warp from the vmap now lands exactly on the grace. **Implication for C1:** disabling the native
  map is NO LONGER coupled to "fix the freeze" (already fixed) — it's purely the UI-simplification /
  single-surface goal, so pick its mechanism on merits, not on the (defunct) menu-context hypothesis.
- **C1 (= M3 interim → M5 cull):** take over the native map. **M3 interim = cover opaque** (vmap over the
  native map on `world_map_open()` — usable, but the native still draws → two maps). **M5 = actually CULL the
  native DRAW** (cover ≠ cull). **⚠ 2026-07-05: both CHEAP levers are DEAD (live-disproven on Linux) — M5
  DEPRIORITIZED:**
  - **D3D12 `RSSetScissorRects` empty-clip (was RECOMMENDED) — DEAD:** the Scaleform map rasterizes full-screen
    through the same generic engine scissors (shadow/mip/full-screen); there is NO map-specific rect (the
    `[SCISSOR]` recon found 0 mapopen=1-only rects; the minimap rect correctly isolated as mapopen=0-only, so
    the tagging was sound). Findings §4c.
  - **GFx `MovieImpl+0xB0` clip write (the pivot) — DEAD:** resolve is correct (`WorldMapDialog+0x140 →
    *(+0x00)=MovieImpl`, validated by `buf==1920×1080`) and the zero-write HOLDS, but the map still renders →
    `+0xB0` is a DESCRIPTIVE viewport, not a render gate. Findings §4d. (Shipped `movieclip read` as a live
    map-viewport diagnostic; hide/show kept as inert scaffolding.)
  - **What's left (all not-cheap):** the Scaleform DRAW-vfunc no-op (Windows Ghidra — "several uncertain runs"),
    a movie/player visible/enable flag (risky RPM field-scan spike, reuse the `movieclip` scaffolding), or the
    CSMenuMan draw-loop skip. Plus the separate engine tile canvas (`CANVAS_SINGLETON` 0x47ef360). Keep the
    Dialog STATE open so pause/input/close stay free.
  - **Net:** production cull is gated on Track A parity + Track B fast-travel anyway (not met), so the cull is
    BOTH not-cheap AND not-urgent → keep advancing the migration on ungated bricks; do the cull from the
    Windows/RE track when it has a slot.
- **C2: RESOLVED (see Locked decisions)** — no hardcoded key. The vmap opens on `world_map_open()` (game menu
  state) → remap/gamepad-agnostic for OPEN, for free. Gamepad NAV of the vmap = feed the polled XInput into
  ImGui nav (M1, no RE); remap-aware action triggers = `windows_keybinding_config_re_prompt.md`.
- **C3:** collapse surfaces — delete the native-map marker-draw path (`proto` branch), retire the
  native-pin suppression knobs, flip the minimap gate from `world_map_open()` to `virtual_map_open()`.
- **✅ CONVERTER RESIDENCY under the cull — VERIFIED SAFE (2026-07-05).** The vmap's underground/DLC
  projection (Fork 2) + the whole live-projection path depend on the engine `WorldMapViewModel` converter
  (`worldmap_probe::project`). **The M5 cull does NOT break it:** (a) the recommended draw-removal is the
  **D3D12 `RSSetScissorRects` empty-clip** (commit `2208332`), which hides PIXELS at rasterize but KEEPS the
  menu update/logic tick → the Dialog+VM stay live — strictly LESS teardown than a full map close; (b)
  `find_view_model()` CACHES the VM (`static s_vm`) and it PERSISTS past a full close, proven by
  `tools/rpc_tests/test_converter_residency.py` (`proj` RPC returns IDENTICAL u,v with the map fully closed,
  du=0.0). So culling the draw is safe for projection. When the scissor toggle is RPC-exposed, extend that
  test to toggle it between the two `proj` calls (belt-and-suspenders).
- **★ ENDGAME data cleanup — REMOVE the BAKED LegacyConv — ✅ DONE 2026-07-05.** Deleted the baked
  `LEGACY_CONV` table (`src/generated/goblin_legacy_conv.hpp`) + its nearest-base-point scan branch in
  `goblin_world_position.cpp::project_dungeon_row_to_overworld` AND the sibling scan in
  `goblin_markers.cpp::entry_world_coords`, plus the generator emission (`tools/generate_data.py`). All
  dungeon/UG/DLC projection now folds LIVE from the resident regulation `WorldMapLegacyConvParam` via
  `goblin::legacy_fold` (which carries its OWN exact-block + nearest-base-point lookup off the live param);
  the baked scan was already dead in steady state (`legacy_fold::available()` gates it out once regulation is
  resident, which every caller is) — it only ran in a pre-regulation warm-up window that no caller reaches.
  The warm-up path now `return false` → callers fall back to raw per-area coords / the circle glyph (prime
  directive). Cross-build clean. The safety rationale for the VM-converter half (Fork 2's
  `worldmap_probe::project`) — resident even map-closed/culled — held; `legacy_fold` is the regulation-param
  path and is independent of the M5 gate, so this landed without waiting on M5.
- **GATE:** C1-cull / C3 only after Track A (parity) is all-green.

---

## Track D — Procedural relief background (parallel, non-blocking)

Give the canvas a terrain backdrop under the markers. Two independent sources, both mod-agnostic:
- **D1 — ART tiles** (map_tile_loading slice 3): stream the real ER map ART seamlessly. Prettiest,
  but it's the baked ER art (Convergence-trap: wrong under a mod that reshapes the 2D map).
- **D2 — raycast heightfield** (RE DONE, `windows_terrain_raycast_heightfield_re_findings.md`): per
  grid cell, cast a down-ray `FUN_140c70360(ctx, 0x5e, start, {0,-H,0}, &pt, &nrm, &dist)` on the
  **game update thread** (NOT present — thread-safety §6), read `pt.y` = ground height, `nrm` =
  hillshade. Same world frame as markers (no transform). Sea = heuristic `y < seaLevelConst`. This is
  the TRUE mod-agnostic relief: sampled from whatever 3D world is loaded → correct for any mod.
  Constraint: only loaded regions hit; sample around the player + extend via grace-warp.
  **UPDATE 2026-07-04: D2 UNBLOCKED** — the cast is an ALIGNMENT fix (vector4, `d3ca993`), runs reproducibly
  on the **PRESENT thread** (no game-thread hook needed), sampler done (`f5323e6`), resolve-ctx-once + bench
  (`95ecc56`). Full details: `heightfield_relief_plan.md`. Two known limits there: within-block misses = the
  ±2000 vertical window (widen) + sea (filter 0x5e skips water → classify, don't fix); and loaded-region-only
  is FUNDAMENTAL → the distant visible terrain (LOD, no collision) needs a **far-LOD heightmap** source:
  `far_terrain_heightmap_re_prompt.md`. Near field = the cast; far field = that.
- **Recommendation:** D2 is the strategic one (mod-agnostic, kills the Convergence trap); D1 is the
  quick pretty win for vanilla/ERR. Ship D2 as the base relief, D1 as an optional overlay.

---

## Prioritization (CONFIRMED 2026-07-04 — maps to Roadmap M1–M5 above)

The detailed track order below is unchanged; the **Roadmap** section reframes it as milestones (M3 = the
ship line, cover-opaque; M4/M5 = gamepad + native-cull polish). Track 1 is THE priority over Track 2.

1. **C0** — read the warp-freeze stall dump (needs the user's next warp; already armed). Cheap, unblocks C.
2. **Track A audit** — the parity gate. Read-only, fast, and it's the blocker for everything user-facing.
3. **Track B** — grace warp menu. Self-contained, no RE, high user value, showcases ImGui-only.
4. **Track C1** — disable native map (likely also fixes the freeze). After C0 + enough of A.
5. **Track D2** — raycast heightfield relief (game-thread hook). Parallel; the big procedural win.
6. **C2/C3** — flip the key + delete the native path. LAST, only when Track A is all-green.
7. D1 / A-tail items (underground+DLC tiles, dial) as polish.

## Review checkpoints (what the user signs off on)
- [x] Prioritization / roadmap — **CONFIRMED 2026-07-04: Track 1 is THE priority, M1→M5.**
- [ ] Track A rows — confirm the `?` audit results = the real go/no-go list. **(the M2→M3 gate)**
- [x] Track B grace-menu HOME — **LOCKED: collapsible SIDEBAR on the vmap.**
- [x] Track C2 key binding — **RESOLVED: no hardcoded key; opens on `world_map_open()` (state-based,
  remap/pad-agnostic). Gamepad nav = M1; remap-aware triggers = keybinding RE prompt.**
- [x] Track D base = D2 (raycast) — **CONFIRMED mod-agnostic-first; D2 unblocked. D1 ART = optional overlay.**
