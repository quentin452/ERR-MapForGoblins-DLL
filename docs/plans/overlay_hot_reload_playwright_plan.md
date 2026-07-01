# Overlay hot-reload + AI Playwright loop (plan)

**Status:** scoped, not started. Raised by <user> 2026-07-01: reload ONLY the ImGui overlay
render code while ERR keeps running (no full restart), paired with the already-proposed Route B
debug RPC so an AI agent can script the REAL running game — screenshot, spot a DX or functional
bug in the minimap/worldmap/icons overlay, fix the overlay source, hot-reload just that piece,
re-observe, loop. Windows-only (DLL LoadLibrary/hook work — see `AGENTS.md` platform rule).

Supersedes/extends Route B in [overlay-test-harness](../memory/tooling/overlay-test-harness.md)
(Route A, the offline mock-data harness, is unaffected and can still land independently). Cross-ref
[dx-bugs-backlog](../memory/bugs/dx-bugs-backlog.md), [input-hooks](../memory/tooling/input-hooks.md).

## Ground truth (verified 2026-07-01, before writing this plan)

- `src/goblin_overlay.cpp` (3962 lines) is ONE file holding both the D3D12 Present-hook plumbing
  AND the ImGui content — not pre-split. Draw functions are reasonably isolated already:
  `draw_panel()` (`:1569`, ~1600-line F1 debug panel), `draw_worldmap_markers()` (`:1478`),
  `draw_minimap_hud()` (`:1550`). `hk_present()` (`:3152`–`3781`, MinHook-installed `:3806`)
  intermixes DX12 swapchain/RTV/command-list mechanics with calling those draw functions — this
  is the natural screenshot attach point (Route B), but must stay in the HOST module; only the
  draw functions are reload targets.
- Global state (`g_device`, `g_command_list`, `g_frames`, `g_show`, …) is file-static in
  `goblin_overlay.cpp`. A reloadable overlay module cannot share these as globals — needs a
  context struct passed in per call.
- Data/render separation is PARTIAL, not absent: `src/worldmap/` already separates marker-layer
  data (`MarkerLayer`, `GraceLayer`, `MapEntryLayer`, `QuestNpcLayer`) from `map_renderer.cpp`'s
  drawing — reuse this for worldmap/minimap markers. `draw_panel()` (F1 debug UI) does NOT go
  through this interface; it reads live game state inline. Panel hot-reload needs its own
  data-passthrough shim; marker hot-reload mostly rides the existing layer interface.
- No RPC/IPC exists anywhere in `src/` today — Route B's debug pipe is greenfield, not a reuse.
- `dx-bugs-backlog` items 11/12 are NOT good first loop targets: item 11 (double-draw) is root-
  caused as two DLL variants loading simultaneously (deploy hygiene, not a code bug — a runtime
  mutex guard is the only open TODO); item 12 is already ✅ fixed (`b10e50e`, `2854600`). Pick a
  live item off the backlog once the loop exists, don't force these two into it.

## Why phase it this way

Reload-safety and the RPC are independent risks; landing them separately means a failure in one
doesn't block the other, and Phase 1 (pure extraction, no behavior change) is verifiable without
any new infra — it's the correctness gate for everything after it.

## Phases

**Phase 1 — extract overlay draw layer behind an interface (no reload yet).**
Pull `draw_panel`/`draw_worldmap_markers`/`draw_minimap_hud` (+ whatever they transitively touch)
behind a call signature that takes an explicit context struct (device/command-list handles,
ImGui context pointer, marker-layer data, panel state) instead of reading file-statics directly.
Land this as a pure refactor — build + run, confirm zero visual/behavior change before touching
reload mechanics. This is the correctness gate: if the interface leaks a hidden global, hot-reload
will silently misbehave later and be hard to attribute.

**Phase 2 — split into a reloadable module + host-side reload mechanism.**
Move the extracted draw layer into its own DLL (e.g. `goblin_overlay_render.dll`), loaded via
`LoadLibrary` from the host (hook) DLL. Host keeps `hk_present`/device ownership, calls through a
thin vtable/function-pointer table resolved via `GetProcAddress`. Dev-only file-watcher (mtime
poll on the render DLL, gated by a config flag) triggers `FreeLibrary` + rebuild + `LoadLibrary` +
rebind on change — no ERR restart. Key risks to solve here, not defer:
  - **ImGui context sharing across the DLL boundary.** Both DLLs must share the SAME `ImGuiContext*`
    (`ImGui::SetCurrentContext` on entry to every cross-DLL call) and be built against the same
    ImGui version/config (`IMGUI_USER_CONFIG`, static/shared CRT) or vtable layouts silently diverge.
  - **D3D12 handle lifetime across reload.** Device/command-list/heap pointers stay live in the
    host; only the draw code swaps, so this should be safe by construction — verify no handle is
    accidentally captured as a `static` inside the render DLL.
  - **Threading.** Reload must happen off the Present-hook thread (or gated with a lock) — never
    `FreeLibrary` a module while its code is on the call stack.

**Phase 3 — Route B debug RPC.**
Named pipe or loopback socket, dev-only, behind the existing config-gate pattern. Commands:
`open_f1`, `search "<item>"`, `set_scale`, `toggle_cluster`, `screenshot` (framebuffer grab off
`hk_present`), plus the new `reload_overlay` command wired to Phase 2's mechanism. External Python
driver gives script→observe against the real running game.

**Phase 4 — wire the AI iterate loop.**
Script: RPC `screenshot` → (agent) inspect PNG, diagnose DX/functional bug → edit overlay source →
rebuild ONLY the render DLL → RPC `reload_overlay` → RPC `screenshot` → verify fix, no regression
elsewhere. First real target: pick a live, actually-code-fixable item off
[dx-bugs-backlog](../memory/bugs/dx-bugs-backlog.md) once Phases 1–3 land (not items 11/12, see
Ground truth above).

## Non-goals

- Does not touch input hooks (`src/input/*`) — those stay host-side, no reload need identified.
- Does not replace Route A (offline mock-data harness) — that's for layout/clutter iteration with
  zero game dependency and can land on its own schedule.
- Not cross-platform: LoadLibrary/FreeLibrary + MinHook is Windows-only; build/test this on the
  Windows dev box per `AGENTS.md`'s platform rule.
