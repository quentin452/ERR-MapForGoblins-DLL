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

| # | Capability (native map / F1) | vmap today | Blocker? |
|---|------------------------------|-----------|----------|
| A1 | Pan / zoom | ✅ canvas | no |
| A2 | Real map ART tiles (overworld) | ✅ slice 2/3a (M00) | partial: seamless full-map streaming = map_tile slice 3 |
| A3 | Underground (M01) + DLC (M10/M11) tiles | ⚠️ group switch exists; tiles not all wired | **yes** |
| A4 | All marker categories (graces/sites/bosses/loot/…) | ✅ reuses native draw (`draw_marker_glyph`) | no |
| A5 | State-aware icons (discovered/collected/cleared/rune glow) | ✅ `563a00e` | no |
| A6 | Marker tooltips (name + count) | ✅ | no |
| A7 | Region name labels | ? audit | ? |
| A8 | Clustering / pile "×N" | ? audit | ? |
| A9 | Item search + "locate" pan | ? audit (F1 search targets the native map today) | **yes** |
| A10 | Fast-travel to grace (double-click) | ✅ but FREEZES — Track C0 | **yes** |
| A11 | Player position marker + heading | ? (minimap has it; vmap?) | ? |
| A12 | ERR day/night dial (ERR-only) | ⚠️ native-map overlay only | ERR-only, low prio |
| A13 | Map cursor → world readout (dev) | ✅ probe | no |
| A14 | UI exclusion zones | native-map overlay only → **moot** after switch | drop |
| A15 | Legacy-dungeon / sub-area maps | ? audit | ? |

**Action:** one audit pass (read-only) resolves every `?` and turns this into the real go/no-go list.

---

## Track B — Grace warp menu (NEW feature; user request 2026-07-04)

A grace LIST menu (not just clicking dots on the canvas), so the player browses and travels:
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

- **C0 (do first):** read the load-watchdog stall dump from the vmap warp freeze → confirm the
  menu-context hypothesis. Decides the takeover mechanism (force-close vs block-open).
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
