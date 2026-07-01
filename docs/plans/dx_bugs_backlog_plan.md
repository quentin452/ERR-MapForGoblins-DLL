# DX Bugs and QoL Backlog Implementation Plan (v2)

The 9 backlog DX/bug items for the MapForGoblins overlay:
1. **Icon visibility** — systemic: icons blend into the ER world map behind, OR are too small.
2. **Native controller bug** — ER doesn't recenter the cursor on mouse→controller switch.
3. **F1 on controller** — no gamepad binding to open the menu (can't play end-to-end on pad).
4. **In-game pause** — bundle a pause feature into the mod (its own F1 checkbox).
5. **Pause on F1 open** — optional auto-pause while the settings menu is open.
6. **Cursor desync** — recenter the cursor on map reopen/restart so tooltips align.
7. **Y-offset (altitude)** — cue when an icon is at a different elevation than the player.
8. **Improved clustering** — tile/spatial-grid clustering instead of the nearest-grace heuristic.
9. **Adaptive distance clustering** — make it work with the new tile clustering.

> **v2 note.** Rewritten after an audit. The big changes: this is **split into 5 independent
> PRs** (v1 bundled all 9 into one unmergeably large change); the pause feature **no longer
> hooks `QueryPerformanceCounter` process-wide** (too blunt — prefer patching the game's own
> timestep/pause flag); clustering (items 8–9) is **sequenced after, and reuses, the
> spatial-grid plan**; method names are corrected (the layers build a lazy `cache_` in
> `markers()`, and `push_marker` is a free function — there is no `GraceLayer::collect` /
> `MapEntryLayer::push_marker`); and the controller/keybind work **reuses the existing
> `parse_gamepad_combo` + `GamepadMask` IniType** rather than reinventing parsing.

---

## 0. Sequencing — 5 PRs, not one branch

| PR | Items | Depends on | Risk |
| --- | --- | --- | --- |
| **A. Icon visibility** | 1 | — | low |
| **B. Y-offset cue** | 7 | — | low |
| **C. Controller + cursor** | 2, 3, 6 | — | medium (input edge-cases) |
| **D. In-game pause** | 4, 5 | needs RE of the pause mechanism | **high — do an RE spike first** |
| **E. Clustering** | 8, 9 | **`feature/spatial-grid-opti`** (grid = the tile buckets) | medium |

Land A/B/C in any order; D after its RE spike; **E only after the spatial grid ships**, so
it reuses the grid cells instead of inventing a parallel tile index.

> **EAC / offline.** All of this (hooks, memory writes, the pause patch) is offline-only with
> Easy Anti-Cheat disabled — already a prerequisite for running MapForGoblins.

---

## 1. Existing infrastructure to reuse (verified)

| Need | Already in tree |
| --- | --- |
| Present / WndProc / SetCursorPos hooks | all in [goblin_overlay.cpp](../../src/goblin_overlay.cpp) (`hk_present`, `hk_wndproc`, `o_set_cursor_pos` — SetCursorPos is **already hooked**) |
| Gamepad combo parsing | `goblin::parse_gamepad_combo("Y+R3")` ([goblin_config.cpp](../../src/goblin_config.cpp) L225) + `IniType::GamepadMask` in [goblin_config_schema.cpp](../../src/goblin_config_schema.cpp) — **already exist**; no `XInputGetState` anywhere yet |
| Keyboard toggle | `config::overlayToggleKey` (VK_F1) used at [goblin_overlay.cpp](../../src/goblin_overlay.cpp) L2820 via `GetAsyncKeyState` |
| Player position | `goblin::get_player_world_pos(x, y, z)` ([goblin_inject.hpp](../../src/goblin_inject.hpp) L57) |
| Marker fields | `Marker` has `worldX/worldZ` only ([marker_layer.hpp](../../src/worldmap/marker_layer.hpp) L28) — `worldY` must be ADDED |
| Marker build site | a free function `push_marker(...)` in [map_entry_layer.cpp](../../src/worldmap/map_entry_layer.cpp) L167; grace markers built in `GraceLayer::markers()` into `cache_` |
| Clustering today | `draw_clusters` groups by `cluster_key` (nearest-grace) via `unordered_map<int,vector<int>>` at [map_renderer.cpp](../../src/worldmap/map_renderer.cpp) L533 |

---

## PR A — Icon visibility (item 1)

The bug is **systemic to rendering**, not a DDS-sampling issue (it shows on the CPU atlas
icons too). Two causes: icons too small, and icon colour blending into the world-map art
behind. Fix is contrast + minimum size — **do NOT touch DDS sampling**.

#### [MODIFY] [src/worldmap/map_renderer.cpp](../../src/worldmap/map_renderer.cpp) — `draw_marker`
* **Minimum size:** clamp the half-extent so icons never shrink below readability
  (e.g. `half = max(half, 8.f)`; graces `max(…, 10.f)`).
* **Contrast backing:** draw a **single** dark disc (`AddCircleFilled`, radius `half + 1.5f`,
  alpha ~150–180) behind the icon. One extra primitive — no extra texture bind / draw-call
  per DDS, so no atlas-sampling cost. Optionally a 1px outline instead/as well.
* Keep it config-gated if it changes the look materially, so users can revert.

---

## PR B — Y-offset / altitude cue (item 7)

#### [MODIFY] [src/worldmap/marker_layer.hpp](../../src/worldmap/marker_layer.hpp)
* Add `float worldY = 0.0f;` to `Marker`.

#### [MODIFY] marker build sites (NOT `collect`/member `push_marker` — those don't exist)
* In the free `push_marker(...)` ([map_entry_layer.cpp](../../src/worldmap/map_entry_layer.cpp) L167), set `m.worldY = d.posY;`.
* In `GraceLayer::markers()` where `cache_.push_back(m)` runs ([grace_layer.cpp](../../src/worldmap/grace_layer.cpp)), set `m.worldY` from the live grace `posY`.
* If `LiveGrace` lacks `posY`, add it and populate it from `row.posY` in the grace capture.

#### [MODIFY] [src/worldmap/map_renderer.cpp](../../src/worldmap/map_renderer.cpp) — `draw_marker`
* If `get_player_world_pos(px,py,pz)` succeeds, `diff = m.worldY - py`.
* If `diff > 15.f` draw a small ▲ (higher) badge top-right; `diff < -15.f` a ▼ (lower) badge.
  Tune the threshold; suppress the badge for graces/world features where altitude is moot.

---

## PR C — Controller binding + cursor recentering (items 2, 3, 6) — DONE, MERGED

Implemented on `feat/gamepad-toggle-cursor-recenter`, in-game verified (2026-07-01): `Y+R3` toggle,
pad-switch recenter, map-reopen recenter, and the combo recorder all confirmed working. Two bugs
found and fixed during verification: (1) recorder captured on the first single button read instead
of waiting for release, cutting multi-button combos short — fixed by accumulating the union of
buttons held and finalizing on release; (2) the toggle-combo check ran unconditionally even while
recording, so pressing the current combo (to record its replacement) also flipped `g_user_show` and
closed the panel mid-record — fixed by gating the toggle check on `!g_gamepad_combo_recording`.

#### [MODIFY] [src/goblin_config.hpp](../../src/goblin_config.hpp) / [src/goblin_config_schema.cpp](../../src/goblin_config_schema.cpp)
* Add `overlayToggleGamepad` (uint16_t) wired through the **existing** `IniType::GamepadMask`
  (parsed by the existing `parse_gamepad_combo`, default e.g. `"Y+R3"`). Do not write new
  parsing.

#### [MODIFY] [src/goblin_overlay.cpp](../../src/goblin_overlay.cpp)
* **Gamepad poll (new):** in `hk_present`, `XInputGetState` for pads 0..3; edge-detect the
  `overlayToggleGamepad` combo → toggle the overlay (same path `overlayToggleKey` drives).
  This is the only genuinely new subsystem here (no XInput in tree yet).
* **Input-source tracking (item 2):** keep a `g_last_input_was_gamepad` flag; clear it on
  mouse/keyboard messages in `hk_wndproc`. When pad input arrives and the flag was false,
  flip it and `SetCursorPos` to the window centre (compensates ER's native non-recenter
  bug — this is ER's bug, we can only paper over it).
* **Cursor recenter on map (re)open (item 6):** detect the map-open transition (the existing
  `world_map_open` / cursor-active signal) and `SetCursorPos` to centre so the ImGui cursor
  and ER cursor agree and tooltips land where the ER cursor is. `SetCursorPos` is already
  hooked (`o_set_cursor_pos`), so route through that.

#### UI
* Settings: an interactive **gamepad combo recorder** writing `overlayToggleGamepad`.

---

## PR C-2 — Followup: full gamepad navigation inside the F1 panel (item 3, gap) — NOT STARTED

Item 3 originally asked to "play MapForGoblins end-to-end on a gamepad only." PR C only covers
opening/closing the panel (the toggle combo) — once open, buttons/checkboxes/lists and the item
search bar still require a mouse/keyboard. Two distinct sub-problems:

1. **Widget navigation (buttons, checkboxes, lists).** ImGui has a built-in gamepad nav mode
   (`ImGuiConfigFlags_NavEnableGamepad` + `io.BackendFlags |= ImGuiBackendFlags_HasGamepad`, fed via
   `ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadDpadUp/…/FaceDown, …)` each frame from the same
   `XINPUT_STATE` the toggle poll in `hk_present` already reads — no second XInput read needed).
   D-pad/left-stick to move focus, a face button to activate. Should compose cleanly with the
   existing poll rather than duplicate it.
2. **Search bar free-text entry.** ImGui's gamepad nav does NOT solve typing into `ImGui::InputText`
   — that needs either an on-screen virtual keyboard (D-pad letter grid, most native-feeling but
   more UI work) or falls back to requiring a keyboard for that one widget. Decide the approach
   before implementing; likely its own small design pass rather than a drop-in.

Related but distinct: item 2's other ask (auto-switch on-screen key-hints between
keyboard/gamepad icons based on the last-active input device) — `g_last_input_was_gamepad` from PR C
already tracks exactly that signal, so hint-icon switching is a cheap follow-on once this exists.

---

## PR D — In-game pause (items 4, 5) — RE SPIKE FIRST

> ⚠️ **v1's design (hook `QueryPerformanceCounter` and freeze the returned ticks
> process-wide) is rejected as the default.** Faking the system clock for the whole process
> affects everything that reads QPC (audio, our own timing, the engine's animation/physics
> integration), and a constant tick makes other consumers compute `dt = 0` → potential
> divide-by-zero / stalls. ImGui can be fed its own real `DeltaTime`, but the blast radius
> on the rest of the process is the problem.

**Spike (RE, ~half a day) before committing to an approach.** Investigate, in order of
preference:
1. **Game's own pause / timestep.** Find the field or function the engine uses to advance
   the world simulation (a global game-speed float, or a frame-advance step) and zero/patch
   it while paused. This is how the established ER pause mod works and is far more surgical
   than faking the clock. Use the Ghidra tooling (`ghidra-re-tooling`) to locate it.
2. **Engine pause/cutscene flag.** ER already freezes the world during cutscenes/menus;
   reusing that state (if reachable) gives a clean, engine-sanctioned pause.
3. **QPC hook — only as a last-resort fallback,** and if used, scope it as narrowly as
   possible and validate audio/animation don't break.

#### Config (added regardless of mechanism)
* `pauseInGame` (bool, hotkey-toggleable), `pauseOnMenu` (bool), `pauseKey` (VK_*, default
  `'P'`). Effective pause = `pauseInGame || (pauseOnMenu && overlay_open)`.

#### UI
* F1 checkboxes: "Pause game" and "Pause while this menu is open". Keyboard key-binder for
  `pauseKey`.
* Keep ImGui responsive while paused (feed it real `DeltaTime` from
  `std::chrono::high_resolution_clock` in `hk_present`).

---

## PR E — Clustering (items 8, 9) — AFTER the spatial grid

The current clustering groups by `cluster_key` (nearest-grace heuristic), which leaves many
markers unclustered. The fix is **tile clustering built on `feature/spatial-grid-opti`** —
do not reinvent a parallel tile index here.

#### Depends on: spatial grid shipped
* The grid already buckets markers per map-space tile; **each occupied cell IS a cluster
  pile**. Use the grid's `for_cells_in_rect` to enumerate visible piles directly — O(visible
  cells), and it scales to the >10k markers we expect once the 31 MapGenie categories land
  (see the spatial-grid plan §0).

#### [MODIFY] [src/worldmap/map_renderer.cpp](../../src/worldmap/map_renderer.cpp) — `draw_clusters`
* Add a `clusterMode` config: `"tile"` (new default) vs `"grace"` (legacy nearest-grace).
* Tile mode: draw each cell's pile at the centroid of its members.
* **Adaptive distance (item 9):** in tile mode, compute distance from the player to the tile
  centre (from the cell's tile indices) to scale the merge threshold — closer tiles split
  into finer piles, distant tiles merge harder.

---

## Verification Plan

Per PR (each independently testable):
* **A:** zoom out fully — every icon stays readable over busy map art (size floor + backing
  disc); confirm no extra draw-call cost in the `render.worldmap.markers` bench.
* **B:** at a known vertical gap (e.g. the Siofra well lift), markers far above/below show
  ▲/▼; same-level markers show none.
* **C:** `Y+R3` opens/closes F1; mouse→stick recenters the cursor; reopening the map
  recenters it and tooltips align with the ER cursor.
* **D:** with the chosen mechanism, enemies/animations freeze while ImGui stays responsive;
  **verify audio and animation don't glitch** (the QPC-hook failure mode). Test pause-on-menu
  and the hotkey independently.
* **E:** toggle tile vs grace clustering — in tile mode no markers are left stranded; pile
  counts are stable while panning; adaptive threshold behaves as you move. Re-run the
  spatial-grid bench at ~8k and a ~12k scale test.

---

## Reconciliation vs the live bug inventory (2026-06-30)

This plan was written against items **1–9**. The source of truth — `docs/memory/bugs/dx-bugs-backlog.md`
— has since grown to **items 1–14 + Followups F1/F2** (Review-2 added 10–14 on 2026-06-29). Before
starting, fold the newer items in; several are already partly addressed or scoped by other work:

- **Items 11 + 12 + 6 (cursor/input) = the map-exit input-softlock + F1-mouse-dead bug.** Investigated
  2026-06-30; root suspected in the DirectInput buffered key-UP drop while F1 is open + no map-close
  cleanup edge. Full hypothesis + Windows-RE verification steps already written:
  `docs/re/windows_input_softlock_re_prompt.md`. **Fold these into PR C** and treat item 12 (movement
  soft-lock "à vie") as the highest-severity item — do the RE confirm before patching the DI hook.
- **Item 13 (minimap ignores marker-scale + clustering).** The minimap HUD is now benchmarked as
  `render.minimap` (commit d792a3a) — use it to verify any minimap fix. Belongs near **PR E**.
- **Item 1 (icons quasi-invisible / blend / too small). ✅ DONE 2026-06-30** (`feat/dx-icon-visibility`,
  PR A) — legibility pass in `draw_marker`: min on-screen size + a dark backing disc gated to *small*
  item/rep icons only (native map symbols untouched). Config `icon_legibility` / `icon_min_half_px`.
  Visually confirmed. Also dropped the redundant discovered green-check on graces.
- **Items 8 + 10 (clustering / fragment heuristic).** Depend on `feature/spatial-grid-opti`
  (`docs/plans/spatial_grid_opti_plan.md`, now also on master). Sequence **PR E** after that lands.
- **Item F1 (native map icons leak underground; `isDLC*2|isUG` gating).** Tied to baked-atlas removal;
  the `[ICONTIER]` ERR-vs-vanilla audit (see HANDOFF) is the data source for whether this is still live.
- **Items F2, 14** and any others not mapped above: still need a PR home — assign when refreshing.

Nothing here is confirmed fixed yet; the above are scope/cross-links, not closures.
