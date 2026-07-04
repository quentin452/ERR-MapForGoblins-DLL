# Master plan — ImGui-only map (retire the native ER map)

Status: **DESIGN 2026-07-04, user-reviewed — decisions locked (see below).** This is the TOP-LEVEL
sequencing plan. It doesn't restate the mechanism sub-plans — it orders them and adds the two things
that were missing: a hard **parity gate** and the **grace warp menu** feature.

## Locked decisions (user review 2026-07-04)
- **Priority:** the proposed order below (C0 → A → B → C1 → D2 → C2/C3 → polish). Parity is the gate.
- **Grace menu home (Track B):** a **collapsible SIDEBAR on the vmap** (list + canvas in one surface).
- **Relief base (Track D):** **D2 raycast heightfield** (mod-agnostic) as base; D1 ART optional overlay.
- **Map key (Track C2): OPEN — user to verify first** whether ER ships custom controller/keybind
  settings, because hardcoding `,`/`m` could break under a rebind. → pushes toward the
  **native-map-edge reuse** (rebind/gamepad-safe, no hardcode); explicit key only a fallback. Do NOT
  wire C2 until the user confirms ER's keybind behaviour.

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
| A3 | Underground (M01) + DLC (M10/M11) tiles | ⚠️ **PARTIAL** — `virtual_map_load_lod(dim,…)` is dim-generic + RPC-reachable, but the UI button hardcodes dim 0 (`panel_virtual_map.cpp:407`) and placement is offset-broken (name-grid≠converter grid, KNOWN GAP `:301-307`) | **yes** |
| A4 | All marker categories (graces/sites/bosses/loot/…) | ✅ reuses native draw (`draw_marker_glyph`) | no |
| A5 | State-aware icons (discovered/collected/cleared/rune glow) | ✅ `563a00e` | no |
| A6 | Marker tooltips (name + count) | ✅ | no |
| A7 | Region name labels | ✅ **DONE** — `panel_virtual_map.cpp` region-label block (`marker_world_pos`→`w2s`, group-gated, `Labels` toggle) | no |
| A8 | Clustering / pile "×N" | ❌ **MISSING** — vmap plots every marker raw (`:589-594`), no piles/counts | **yes** (dense areas unreadable) |
| A9 | Item search + "locate" pan | ❌ **MISSING** — F1 search targets the native map only; vmap ignores it | **yes** |
| A10 | Fast-travel to grace (double-click) | ✅ but FREEZES — Track C0 | **yes** |
| A11 | Player position marker + heading | ✅ **DONE `69188c3`** — dot/heading-arrow on the vmap (base ER + matching group), minimap yaw convention through the vmap axis signs | no |
| A12 | ERR day/night dial (ERR-only) | ❌ MISSING — native-map overlay only (`map_renderer.cpp:1170`) | ERR-only, low prio |
| A13 | Map cursor → world readout (dev) | ✅ probe | no |
| A14 | UI exclusion zones | native-map overlay only → **moot** after switch | drop |
| A15 | Legacy-dungeon / sub-area maps | ❌ MISSING — vmap has only the 4 top-level groups (`s_group`, `:593`); dungeons folded into overworld | maybe (dungeons still show as folded markers) |

**A-BUILD list (the gate) — ordered cheapest/most-critical first:**
1. ~~**A11 player marker + heading**~~ ✅ DONE `69188c3`.
2. ~~**A7 region labels**~~ ✅ DONE — `panel_virtual_map.cpp`: per-anchor `MAJOR_REGION_ANCHORS` →
   `marker_world_pos`(conv_underground) → `w2s`, gated on `active_world==0 && group==s_group`, gold
   text + shadow + pill (same aesthetic as native `draw_region_labels`), `Labels` checkbox toggle.
   Non-interactive (the native click-to-hide chip is native-only). Rides the SAME proven marker
   transform, so it co-locates with its region's markers by construction. Builds clean.
3. **A8 clustering/piles** (reuse the spatial-grid clustering from map_renderer, or a vmap-local pass).
4. **A9 item search/locate** (make the F1 search ring + pan target the vmap too).
5. **A3 tiles underground/DLC + placement fix** (map_tile slice 3; the offset gap).
6. A12 dial (ERR-only, low), A15 legacy-dungeon sub-maps (decide if needed).

**vmap UX backlog (from live testing 2026-07-04):**
- ✅ grace z-order (draw on top) `dd64d8d`; ✅ hover z-order (grace wins the tooltip/warp) `89d0cd8`;
  ✅ focus-player-on-open `cd7948b`; ✅ grace warp id fix (bonfireEntityId) `89d0cd8`.
- **Spiderify on non-clustered overlapping markers** — spiderify already ships on CLUSTERS; extend it
  to plain overlapping markers on the vmap (spread them on click/hover so each is selectable). Later.

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
- **C1:** disable the native map (single_surface slice 1). Expected side effect: the warp freeze
  disappears (fires from gameplay state, like RPC).
- **C2:** bind the open key. ER map key = **`,`** on AZERTY / **`m`** on QWERTY (same physical key).
  Open the vmap on that key regardless of layout; native map suppressed. (The vmap already auto-opens
  on the native-map edge — reuse that trigger so any rebind/gamepad works; the explicit key is the
  fallback.)
- **C3:** collapse surfaces — delete the native-map marker-draw path (`proto` branch), retire the
  native-pin suppression knobs, flip the minimap gate from `world_map_open()` to `virtual_map_open()`.
- **GATE:** C2/C3 only after Track A is all-green.

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
- **Recommendation:** D2 is the strategic one (mod-agnostic, kills the Convergence trap); D1 is the
  quick pretty win for vanilla/ERR. Ship D2 as the base relief, D1 as an optional overlay.

---

## Prioritization (proposed — REVIEW)

1. **C0** — read the warp-freeze stall dump (needs the user's next warp; already armed). Cheap, unblocks C.
2. **Track A audit** — the parity gate. Read-only, fast, and it's the blocker for everything user-facing.
3. **Track B** — grace warp menu. Self-contained, no RE, high user value, showcases ImGui-only.
4. **Track C1** — disable native map (likely also fixes the freeze). After C0 + enough of A.
5. **Track D2** — raycast heightfield relief (game-thread hook). Parallel; the big procedural win.
6. **C2/C3** — flip the key + delete the native path. LAST, only when Track A is all-green.
7. D1 / A-tail items (underground+DLC tiles, dial) as polish.

## Review checkpoints (what the user signs off on)
- [ ] Prioritization order above.
- [ ] Track A rows — confirm the `?` audit results = the real go/no-go list.
- [ ] Track B grace-menu HOME (vmap sidebar vs F1 tab vs canvas button).
- [ ] Track C2 key binding (`,`/`m` layout handling) — confirm before wiring.
- [ ] Track D base = D2 (raycast) vs D1 (ART) — confirm the mod-agnostic-first call.
