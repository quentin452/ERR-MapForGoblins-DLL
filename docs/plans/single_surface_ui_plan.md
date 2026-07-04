# Plan — single-surface UI: disable the native ER map, collapse to Minimap → Virtual World (+ F1 bundle)

Status: **DESIGN 2026-07-04.** Slice 0 (load watchdog) DONE. Slices 1–3 scoped, blocked on the
watchdog verdict + one RE gap (safe menu-close). Fork an impl branch from `master` when slice 1 starts.

Related: `virtual_world_multi_world_design.md` (open-via-M, slice D done), `map_tile_loading_plan.md`
(the ART on the canvas), `docs/re/windows_loading_screen_state_re_findings.md` (the watchdog RE).

## Goal (user, 2026-07-04)

> "le but est de désactiver complètement la map original ER … au lieu d'avoir F1, Worldmap ER,
> Minimap, Virtual world → juste avoir Minimap et quand on appuie sur une touche → transition
> Virtual world (avec un bundle F1 par-dessus)."

Collapse four UI surfaces (F1 panel · native ER Worldmap · Minimap · Virtual World) to **two states**:

- **Gameplay:** Minimap HUD only.
- **Map key pressed:** transition to the **Virtual World** full page; **F1 panel overlaid on top**.

The native ER world map is **never shown**. This deletes the whole "draw our markers on top of the
game's map" path and its duplicated if/else — the Virtual World becomes the sole map surface.

## Current wiring (measured 2026-07-04)

One native-map signal drives everything: `goblin::world_map_open()` = `CSMenuMan+0xCD == 7`
(`goblin_tutorial_popup.cpp:336`). Consumers:

| Consumer | file:line | role today |
|----------|-----------|-----------|
| vmap auto-open | `panel_virtual_map.cpp:352-356` | open vmap on the native-map-open EDGE when a custom world is active (slice D) |
| minimap suppression | `map_renderer.cpp:2512` | hide minimap while the native map is open |
| native-marker draw gate | `goblin_overlay_render.cpp:206` | draw our markers ON the native map only while it's open |
| WndProc input routing | `input_wndproc.cpp:219-232` | route mouse to ImGui / consume chip clicks while native map open |

The mod **never blocks the map key** — it is a passive observer (`input_wndproc.cpp` only *consumes*
input once an overlay is already open; nothing intercepts `WM_KEYDOWN` for the map key or the
menu-open path). Per-frame dispatch `goblin_overlay.cpp:1905-1914`: F1 panel (`g_show`), vmap
(self-gates on `virtual_map_open()`), native-map markers (`proto=true` always), minimap (`showMinimap`).

## The warp-freeze connection — DISPROVEN (2026-07-04; the freeze was unrelated data bugs)

**RESOLVED and decoupled from this plan.** The vmap warp freeze turned out to be TWO data bugs in the
grace layer, NOT menu context: (1) it warped by the BonfireWarpParam ROW KEY (ERR-remapped) instead of
`bonfireEntityId` (`89d0cd8`), and (2) the warp offset was the CT's `-1000` instead of `0` (`088aabc`).
Both fixed; the vmap warp now lands exactly on the grace with the native map open. So disabling the
native map is **no longer justified by "fixing the freeze"** — it stands purely on the UI-simplification
goal. The (now historical) hypothesis is kept below for context.

vmap double-click warp FREEZES the loading screen; the SAME `warp::to_grace` via debug-RPC works.
Only difference at the (now identical, post-frame) call site: **RPC fires from gameplay (no menu
open); the vmap warp fires with the native map menu still open underneath** (vmap auto-opened on the
map edge but the native map was never closed). Warping while the map menu is mid-teardown very
plausibly hangs world-streaming.

If true, disabling the native map (slice 1: force it closed so the vmap is the surface, not an
overlay on top of a live menu) makes the vmap warp fire from a gameplay-like state — **exactly like
RPC** — and the freeze goes away for free. **Slice 0's watchdog confirms/denies this** by dumping the
stuck threads on the next freeze (menu-teardown frames in the stack = confirmed).

## Slices

### Slice 0 — load watchdog (DONE, `cb2eb0b`)
`goblin_load_watchdog`: watches `LocalPlayer==null`, armed by `warp::to_grace`; on a stuck load →
`logs/MapForGoblins_load_stall_<pid>.txt` + all-thread minidump + target grace. Gives the freeze
diagnosis the freeze watchdog can't (present keeps beating on a loading screen). → read the .dmp
stacks to confirm the menu-context hypothesis above before committing to slice 1's close mechanism.

### Slice 1 — native-map takeover (the core; blocked on RE: safe menu-close)
Make the native map **never render as an interactive surface**. Two candidate mechanisms — pick by
the watchdog's stack evidence + a spike; do NOT blind-write menu state (that risks the same
menu-stack corruption class = more freezes):
- **(a) Force-close on the open edge.** On `world_map_open()` rising edge, call the game's
  map-menu CLOSE path (RE gap: the close function / the input that maps to "back out of map"),
  then open the vmap. Cleanest if the close call exists and is safe to invoke programmatically.
- **(b) Block the open.** Hook the menu-open path that sets `CSMenuMan+0xCD=7` for the world-map
  menu id and no-op it, opening the vmap instead. More robust (respects rebinds/gamepad, no flash)
  but needs the menu-open function RE'd.
- **(c) Render-suppress only.** Keep the native map "open" as the input/menu context but suppress
  its rendering, drawing the vmap on top. Lowest risk to the menu stack, BUT keeps the menu open
  during warp → does NOT fix the freeze (contradicts the hypothesis). Fallback only if (a)/(b) prove
  unsafe and the freeze turns out to be unrelated to menu context.

Trigger stays the existing native-map EDGE (`panel_virtual_map.cpp:353`) so any map keybind /
gamepad works — no new key interception needed. Add config `map_takeover` (bool, default gated
until verified) so it can be turned off if a mechanism misbehaves.

### Slice 2 — verify warp from the vmap (gated on slice 1)
With the native map disabled, re-test the vmap double-click warp. Expected: no freeze (fires like
RPC). If the watchdog still trips → the freeze is NOT menu-context; use the .dmp to find the real
cause (streaming target? bad grace arg from the vmap path vs RPC path?).

### Slice 3 — collapse the surfaces + retire the native-map draw path
Once the native map is gone as a surface:
- Delete the "draw markers ON the native map" path (`goblin_overlay_render.cpp:206`, the `proto`
  branch `goblin_overlay.cpp:1911`) — markers only ever draw on the vmap now.
- Native-pin suppression (`graceSuppressNative`/`landmarkSuppressNative`/`suppressNativeBosses`,
  `clipGameUi`) becomes moot (nothing native to suppress) → retire those knobs.
- F1 panel: keep as the "bundle overlaid on top" (open while vmap is up); confirm the WndProc
  routing (`input_wndproc.cpp:170-232`) does the right thing when BOTH vmap + F1 are up.
- Minimap stays the sole gameplay HUD; its suppression gate flips from `world_map_open()` to
  `virtual_map_open()`.

## Open questions
- **RE gap:** the map-menu close function / open path (slice 1 (a)/(b)). Resolve on Linux via the
  debug-RPC find-what-accesses on `CSMenuMan+0xCD` during a real map open/close.
- Does F1's `overlayToggleKey` need re-scoping so the map key and F1 key don't collide when both
  overlays can be up? (Currently independent; verify.)
- Multi-world: when a custom virtual world is active vs the default Lands Between — the takeover must
  respect `goblin::vworld::active()` (already the auto-open gate).

## Risks
- Writing menu state directly (option (c)-ish shortcuts) can corrupt the CSMenuMan stack → freezes.
  Prefer calling the game's own close/open path (a/b) over poking `+0xCD`.
- Removing the native-map draw path is one-way; keep it behind `map_takeover` until slices 1–2 are
  in-game verified across overworld + underground + DLC.
