# Virtual multi-world — architecture design (framework-owned worlds)

Design for MapForGoblins' custom "dev worlds" (World Virtualization vision #1): the framework holds N of
its OWN worlds and shows the active one's map + markers. Answers 4 architecture questions raised
2026-07-04. Complements `docs/re/worldmap_new_page_spike_findings.md` (native page = unsolved write
frontier → we go MOD-OWNED) and `docs/runtime_modding_framework_vision.md`.

## The unifying fact (how ER's map works)
ER's world map ART is **`WorldMapTile` sheets** — BAKED DDS texture tiles (`tileId = group*10000 +
gridX*100 + gridZ`), streamed per-view, **NOT generated at launch**. `group 0/1/2 = overworld / underground
/ DLC` are **separate sheets + separate converter slots + separate coordinate spaces** — i.e. ER already
uses a "dimension"-like separation per page. Consequence: overworld and DLC do NOT share map coordinates,
and we CANNOT generate ER-style tiles — a custom world supplies its OWN map image (or a procedural grid).

## Decision 1 — Collision avoidance: the FRAMEWORK assigns position, never the player
Authors place content **relative to their world's origin**; the framework maps that to absolute coordinates
in a reserved region. Two levels:
- **Marker-only world (current virtual page):** each world = its OWN mod coordinate namespace (never the
  engine's space) → collision is structurally impossible; the framework just keeps worlds in separate
  namespaces (trivial — a per-world origin).
- **Walkable world (future, needs ADD geom):** the framework assigns a **reserved mapId / area block** per
  world (ER's own dimension mechanism — the same way underground/DLC are physically separate map regions).
  Geometry is placed into that reserved dimension, so it never collides with base maps or other worlds.
  The player NEVER picks an absolute position — they pick a world + a world-relative spot; the framework
  resolves it.

## Decision 2 — "Which world am I in": a framework ACTIVE-WORLD state
The framework tracks an **active world id** (default = base ER). Source of truth:
- **Engine-backed (walkable) world:** the player's live **mapId** (`PLAYER_MAPID_SLOT`, already RE'd) →
  mapId→world lookup. Entering a world's reserved mapId auto-sets active world.
- **Marker-only world:** an explicit framework selection (activate a bundle via UI/RPC). You are physically
  still in some ER mapId, but the framework's active world decides which world's map+markers to show.
The virtual page reads the active-world id and renders that world.

**⚠ FOLLOWUP (when worlds become walkable) — reconcile `active_world`, `s_group` and PlayerDim.** Today the
vmap uses THREE separate axes that only coincide because custom worlds are marker-only:
- `active_world` (`vworld::active()`) — which world's DATA is shown (0 = Base ER). Gates relief / item-search
  / ER markers / player-dot on `== 0` (correct: a marker-only custom world has no ER terrain/coords).
- `s_group` — which dimension PAGE is displayed (0 OW / 1 UG / 2 DLC-OW / 3 DLC-UG). MANUAL selector.
- **PlayerDim** — the player's physical dimension (`get_player_dimension_area()`, RE'd; goblin_world_position.cpp).
  Used for the player-dot (`pgroup == s_group`) + focus-player.
While custom worlds aren't walkable, the player is ALWAYS in real ER ⇒ `active_world == 0` ⟺ "the player's
world", so the gates are correct. Once a walkable custom world exists (reserved mapId, per Decision 1), these
diverge: `active_world != 0` while the player is PHYSICALLY in that custom dimension → the `active_world == 0`
gates must instead key off "is the active world engine-backed / does it own ER-frame data", and PlayerDim
should drive the active-world auto-set (mapId→world). Also a standalone enhancement (nice even before walkable
worlds): auto-follow `s_group` to PlayerDim so the vmap page switches to UG/DLC when the player crosses
dimensions (today only the one-shot "focus player" does that). Both are RE'd already — just wiring.

## Decision 3 — Open with "M" (the game map key), not F1
Production UX: the virtual world map is what **M** shows when the active world is a custom world, NOT a
separate dev window.
- Hook the worldmap-open (already RE'd: `worldmap_open()` + the dialog/page state).
- When M opens the map AND active world is virtual → draw the MFG virtual page in place of / over the native
  Scaleform map (suppress or ignore the native map render for that frame set).
- Keep the current `vmap` RPC + the F1 Dev-tab toggle as a **DEV harness** (drive/inspect without being in
  a custom world). Slice A's window is that harness; production = M-triggered.

## Decision 4 — The map surface for a custom world
Since we can't bake ER tiles, a custom world's map is a MOD-DRAWN surface (slice A canvas): a supplied
background image (per-world, from the bundle) OR the procedural reference grid, plus mod-projected markers.
Per-world projection (origin/scale) lives in the world's bundle.

## Where this lands in the slices (updates the virtual-page plan)
- ✅ Slice A — the mod-drawn canvas (pan/zoom/grid). DONE.
- Slice B — draw the active world's markers on the canvas (mod projection).
- Slice C — the WORLD model: an active-world id + per-world {origin/scale, marker set, optional bg image},
  bundle-backed (extends `goblin_world_bundle`). Marker-group tagging (synthetic group ≥100) so markers
  belong to a world. Framework assigns coordinate namespaces (Decision 1, marker level).
- Slice D — M-integration (Decision 3): open the virtual page from the game map key when active world is
  virtual; the F1/`vmap` path stays as the dev harness.
- Later (walkable, needs ADD geom + a reserved-mapId allocator) — Decision 1's dimension level + Decision 2's
  mapId→world. Blocked on the geom-spawn ADD frontier (pivot 2, Windows RE).

## ★ Endgame: TOTAL native-map replacement (the ultimate form) — phased + risk-scoped (2026-07-04)
Vision: the mod-drawn map REPLACES ER's native worldmap entirely — one map for base ER AND custom worlds,
killing the two-UX split and the intermittent marker CLIPPING for good. Coherent, but large + risky; the
map is not a viewer — it drives FAST-TRAVEL, beacons, fog/fragments, region labels, DLC/UG page switching.

**Key reframe — the clipping win is FREE, no risky suppression needed.** Clipping comes from drawing our
markers OVER the Scaleform map. A full-screen mod-owned surface (slices A-D) does NOT draw over it — it IS
the map — so clipping is gone the moment the mod map renders full-screen, WITHOUT suppressing the native
map. So the dangerous "replace/suppress native" step is LAST and OPTIONAL; most value lands before it.

**The make-or-break feature = FAST-TRAVEL.** ER's map's #1 job is warp-on-grace-click. We have the
primitive (`warp <graceId>` = LuaWarp_01), but replacing the map means WE must: list discovered graces,
let the user pick one, warp. Getting this wrong is GAME-BREAKING — be ultra-rigorous here (the user's own
caveat). Everything else (beacons, legend, fog reveal, fragments, region labels, page switch) is additive.

**Building blocks (the user's list):**
- **Player position** — DONE (we already project the player point).
- **DCX map-tile loading** — ER's map art = `WorldMapTile` DDS sheets, DCX-compressed. We HAVE Oodle on
  Linux (`liboo2corelinux64.so.9`) — extract DCX → DDS → GPU texture, drawn per-tile at its grid position
  (`tileId = group*10000+gx*100+gz`), scaled by the mod projection. Real work, no RE wall.
- **PNG support** — for custom worlds' own map images (stb_image or similar). Easy.
- **All markers** — DONE (per-group projection on the canvas).
- **Fast-travel** — primitive DONE; the UI + grace-list + click-to-warp is the risky new build.

**Phasing (keep the native map as a FALLBACK until parity):**
1. **Full ALTERNATIVE map** (opened via M, native still available as fallback): mod surface + ER tiles +
   player pos + all markers + **fast-travel** (grace list → `warp`). No clipping (our surface). This is the
   bulk of the value + already sits on the A-D foundation.
2. **Feature parity**: fog/fragments reveal, beacons (player-placed pins), region labels, DLC/UG page
   switch, legend — additive slices.
3. **Suppress the native map** (the actual "replace"): hook map-open so M shows ONLY ours. LAST + riskiest
   + optional — the clipping/UX wins are already banked by phase 1.

**Verdict:** this is a multi-phase CAPSTONE, not a slice — but it's the natural top of the virtual-map work
(A-D are its foundation). Do NOT gate the custom-world track on it. Recommended first brick if pursued:
phase-1 tile loading (DCX→GPU) so the mod map shows ER's real art, then fast-travel. Keep native as
fallback throughout; only suppress it once fast-travel parity is proven.

## Feature-parity with ER's native map (from the button bar) + gamepad (2026-07-04)
The native map's on-screen actions (`E place beacon`, `Q close`, `R marker`, `F sites of grace`, `Z map
menu`, `zoom`) map to a SHORT list once you subtract cosmetics — **almost everything else is cosmetic**:
- **Compass** → already covered by the minimap (`showMinimap`); reuse its heading.
- **Zoom / pan / close** → the vmap canvas already has these (wheel/drag; a close verb).
- **Sites of grace (`F`) → FAST-TRAVEL** — the make-or-break feature (see the endgame section); `warp`
  primitive exists.
- **The REAL functional gaps (not cosmetic):**
  1. **Clock / time-of-day** ("Morning" etc.) — read the live game time-of-day and show it. Small RE (a
     world-time field) + a label.
  2. **Blue ER "you clicked here" marker** — the map's click-to-place location pin (the blue diamond). We
     own the canvas → click → place a transient pin at the `s2w` world coord.
  3. **Custom beacon / personal marker (`E` place beacon)** — a persistent user-placed marker (guidance
     beacon). Store on the active world (bundle-backed, like `vworld` markers) + draw it.
  Everything else in the submenus (legend, map-menu `Z`, fragment/region cosmetics) is polish.
- **GAMEPAD support (design gap — must add):** the vmap canvas is mouse-only today (wheel/drag). It MUST be
  controller-drivable (the game map is): **left stick → pan, triggers/RB-LB → zoom, D-pad/stick → move a
  selection cursor over markers, A → select/place, B → close.** ImGui nav handles buttons/focus but a
  world-space CANVAS needs explicit stick→pan/zoom + a cursor reticle (the game map uses a reticle, not a
  free pointer). Wire it off the same gamepad input the F1 combo already reads. Add to every vmap slice.

## Path-loading REGRESSION risk (vanilla packed/unpacked vs ERR modded) — add RPC checks (2026-07-04)
The asset-load paths differ by install shape: **vanilla PACKED** (base game dvdbnd `Data*.bhd`, Windows-only
reader), **vanilla UXM-UNPACKED** (loose `menu/…` files), and **ERR-modded** (me3 overlay `mod/menu/…` loose
+ base dvdbnd underneath). `read_game_file_decompressed` tries loose→dvdbnd, but the map-tile / name / FMG
paths can shift subtly per shape → a silent regression on one install shape while another still works.
**Add RPC regression checks** that assert the load path resolves on the CURRENT install (e.g. a
`assets_probe` verb: can it resolve `menu/71_MapTile`, an item-icon sheet, an FMG, a param) + record the
install shape — so a path break shows up in the persisted test ledger (below) instead of as a phantom.

## Process: PERSIST RPC test PASS/FAIL + regression detection (DONE 2026-07-04)
`tools/mfg_session.py::summary()` now appends every run to `tools/rpc_tests/results.jsonl` (gitignored) and
LOUDLY flags a regression: a check that PASSED in the prior run of the same test but FAILS now (or overall
pass→fail). So a regression is RECORDED, not a phantom. **`tools/rpc_tests/check_regress.py` (DONE)** scans
the ledger and, per test, compares its two most recent runs → prints REGRESSIONS (PASS→FAIL) / recovered /
new-failing, `--history` audits, `--test` filters; **exit 1 on any regression** so an agent/CI sweep gates on
it. **`tools/rpc_tests/run_all.py` (DONE)** = the nightly sweep. **AGGREGATED to minimize game boots:** the
cold boot (~45s–2min) is the real cost, so single-boot tests that declare a module-level `SWEEP = <_test|test>`
marker run TOGETHER in ONE shared GameSession (each self-loads via `g.load_save()` → isolated, no
contamination — E2E-verified 6 tests, one boot, all pass); the inherently multi-boot / boot-config tests
(`custom_item`/`gapc`/`author_items`, no SWEEP) run per-boot. So the 9-test suite = **4 boots, not 9**.
`--no-aggregate` forces all per-boot. Each test appends to the ledger, then `check_regress.py` runs; exit 1
if any test failed OR any regression. Must run LOCALLY (the game is on this box — a cloud cron can't drive it); Steam must be
up. Nightly local cron (user adds to their crontab):
`30 4 * * * cd <repo> && python tools/rpc_tests/run_all.py >> tools/rpc_tests/sweep.log 2>&1`.
Loop fully closed: run → record → detect → gate.
**Git-tracked status (DONE):** `check_regress.py` also regenerates `tools/rpc_tests/STATUS.md` (a committed
markdown table of the LAST run of each test — ✅/❌, N/total, timestamp) from the local ledger, so the repo
carries a visible, versioned test-status snapshot (the raw `results.jsonl` stays gitignored). `run_all`
refreshes it every sweep; the user/cron commits it (auto-commit left out to avoid daily noise commits).
**Dev ergonomics (DONE):** `.vscode/tasks.json` (git-tracked via a `.gitignore` `.vscode/*` + `!tasks.json`)
gives one-click tasks — Configure, Build DLL (clang), Deploy, Build+Deploy, Run all tests, Check
regressions — so the user compiles + tests without memorizing the ninja/cmake/cp/python invocations.

## Open RE items (small, mostly confirm-live)
- mapId→world lookup + a reserved-mapId/area allocator (only for walkable worlds — not on the marker path).
- The exact M-open hook point to swap in the virtual page (worldmap-open is RE'd; the suppress/overlay of
  the native Scaleform map for a full-screen mod surface needs a probe).

## ★ PIVOT (2026-07-05, user directive): a vworld IS a 3D world — restructure around real dimensions

Reframe that supersedes "world 0 = base ER + groups 0/1/2" above. **A vworld = a real 3D streamable world,
not a 2D map page.** Consequences:

1. **"Base ER" is NOT one world (id 0) — it is MANY 3D worlds.** The overworld (m60), the underground
   (m12), the DLC / Land of Shadow (m61), and each legacy dungeon (m10/m11/m15/m30s/…) are physically
   SEPARATE 3D spaces (they overlap in XZ; they are not sub-pages). The current model conflates them into
   "world 0 with a group toggle" — conceptually wrong.
2. **The player occupies exactly ONE vworld at any instant.** Its identity = the live **mapId**
   (`PLAYER_MAPID_SLOT`, already RE'd: `mapId = *(singleton+0x2c)`, `AA=area, BB=gridX, CC=gridZ`, decoded
   in `get_player_map_pos`). So **`PlayerDim` already exists** — no new RE. `set_active(PlayerDim)` = "show
   the 3D world the player is standing in", which is why hardcoding `set_active(0)` is wrong.
3. **3D-world identity vs MAP-space are different axes.** Many 3D worlds share ONE map projection: the
   overworld + all its legacy dungeons project onto the overworld page (via WorldMapLegacyConvParam); m12
   projects to the underground page; m61 to the DLC page. So a vworld has *a map-space it renders to*, and
   several vworlds can share one. The 3 current "groups" are really the 3 base **map-spaces**, not the
   worlds. A vworld → (mapId range, map-space, projection).

### Restructured model
- **vworld registry keyed by dimension**, not a flat id list. Each entry: `{ mapId-range/predicate,
  map-space (which of the 3 base pages, or a custom surface), projection (origin/scale), marker source }`.
  Base ER contributes several built-in dimension-vworlds (overworld, underground, DLC); custom bundles add
  more (Track-B walkable 3D worlds get a reserved mapId + their own map-space).
- **`PlayerDim` resolver** (cheap, no RE): live mapId → matching vworld. Overworld+dungeons → the overworld
  vworld; m12 → underground; m61 → DLC; a reserved custom mapId → that custom world (Decision 2, engine-
  backed). The map **auto-selects the player's vworld** on open / follows it live.
- **`set_active`** becomes: explicit user pick OR `PlayerDim`. The "Player" button + map-open use `PlayerDim`
  so they recentre on the player IN whatever world they're in — the natural fix for the recenter bug that
  started this (base ER isn't special; it's just "the world the player happens to be in").

### Migration (don't break the working base-ER map)
1. **PlayerDim resolver first** (small): `int player_vworld()` = mapId→dimension using the mapId+group we
   already read. Make the "Player" button + focus use it instead of hardcoded 0.
2. **Promote the 3 base map-spaces to built-in dimension-vworlds** in the registry (overworld/UG/DLC), so
   the world dropdown lists real dimensions + custom worlds uniformly; the group toggle becomes a
   consequence of the active dimension-vworld, not a separate control.
3. **Custom 3D worlds** slot into the same registry (marker-only now; walkable = the ADD-geom frontier,
   Decision 1/2 — reserved mapId allocator, blocked on geom-spawn RE).

Frontier unchanged: marker-only + map-read = doable now; a truly WALKABLE custom 3D world still needs the
MSB/geom-ADD wall (pivot 2). This pivot is about the MODEL (dimensions are worlds) + the cheap PlayerDim
win, not about new walkable worlds.
