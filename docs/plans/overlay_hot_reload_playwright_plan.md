# Overlay hot-reload + AI Playwright loop (plan)

**Status:** Phase 1 COMPLETE for all 3 draw functions (2026-07-01) — slice 1
(`draw_worldmap_markers`/`draw_minimap_hud`) MERGED to `master`; slice 2 (`draw_panel`)
build-verified + IN-GAME CONFIRMED, not yet merged. Phase 2 (DLL split) not started. Raised by
<user> 2026-07-01: reload ONLY the ImGui overlay
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

**Phase 1 coupling audit (2026-07-01, before code moves):** full grep/read pass over the 3 target
functions found most of their direct-global touches are already self-contained (`overlay_layers()`,
`ensure_grace_srv()`/`ensure_grace_dungeon_srv()` — own file-static state, called only from inside
these 3 functions, safe to move wholesale in Phase 2). The genuinely HOST-shared globals (written
by code outside the 3 functions — `hk_present` body, icon-harvest, init) are: `g_hwnd`,
`g_nav_frames`, `g_gamepad_combo_ready`, `g_item_icon_srvs`, `g_grace_state`. These must stay as
pointers/refs into the host statics inside any context struct, never copies, or host and draw layer
diverge. `draw_panel`'s ~1600-line body additionally owns a large panel-only UI-state cluster
(`g_large`, `g_grace_dbg_*`, gamepad-combo strings) — draw_panel is sole writer for most of it, a
clean fit for a `PanelCtx` sub-struct, but NOT yet audited to the same PR-boundary precision as
`draw_worldmap_markers`/`draw_minimap_hud` (same caveat pattern as the inject-refactor plan's own
"not yet audited" sections before PR 4). 26 function-local `static`s in `draw_panel` (incl. the
locate hold-frame counters at `:1506-1508`) are function-local, not call-site state — out of scope
for Phase 1, only become a real risk in Phase 2 (DLL reload resets statics on `FreeLibrary`).

**Phase 1, slice 1 — DONE, build-verified, awaiting in-game confirm (2026-07-01, `feat/overlay-draw-context`):**
scoped to the two small, audit-confirmed self-contained functions first (cleanest/lowest-risk,
same "biggest/cleanest win first" convention as the inject-refactor plan). Added `OverlayFrameCtx`
(`atlas_srv`, `hwnd`, `nav_frames`) right above `draw_worldmap_markers`; both `draw_worldmap_markers`
and `draw_minimap_hud` now take `const OverlayFrameCtx &` instead of reading `g_atlas_gpu`/
`g_atlas_ready`/`g_hwnd`/`g_nav_frames` directly. `hk_present` builds one `frame_ctx` per frame and
passes it to both call sites. `draw_panel` is UNCHANGED in this slice — its own coupling (panel-UI
cluster + host-shared globals above) needs its own PR-boundary audit pass before touching, same as
the inject plan treated its biggest/messiest section last. Cross-build (clang-cl+xwin) clean, only
pre-existing unrelated warnings (codecvt deprecation, ImGui memset-on-non-trivial). Deployed +
**IN-GAME CONFIRMED 2026-07-01**: `[SIG]` 29/29 clean, atlas loaded, `render.minimap` bench firing
every frame for the whole session, no crash/error — Phase-1 correctness gate passed for these 2
fns. **MERGED to `master`** (fast-forward, `feat/overlay-draw-context` deleted post-merge).

**Phase 1, slice 2 — `draw_panel` coupling audit DONE (2026-07-01), code not started.**
Full 4-chunk read of `draw_panel` (`:1581-3159`, ~1580 lines) cross-checked with repo-wide grep.
Found 13 globals + 1 genuinely-split helper cluster (bigger in volume than slice 1's 3 fields, same
kind of split): `g_large` (panel-owned, safe) and the grace-debug-override family (`g_grace_dbg_srgb`/
`g_grace_dbg_swiz`/`g_grace_dbg_fmt_used`/`g_grace_state`/`g_grace_gpu`+uv/`g_grace_dbg` vector —
panel is sole direct writer of the two override ints, rest round-trips through the already-self-
contained `ensure_grace_srv`/`ensure_grace_debug` helpers) both fit a panel-owned `PanelCtx`. Host-
shared (must stay pointer/ref, NOT panel-owned): gamepad-combo recording UI (`g_gamepad_combo_recording`/
`_ready`/`_reject_reason`, also touched by `hk_present`'s XInput poll), `g_nav_frames` (reuse slice-1's
existing `OverlayFrameCtx` field), and the D3D12/atlas render-infra cluster (`g_atlas_ready`/
`g_atlas_gpu` — same as slice 1 — plus a NEW riskier find: `g_device`/`g_command_queue`/`g_srv_heap`/
`g_next_item_srv`/`g_pending_icons`/`g_icon_batch_open`/`g_item_icon_srvs`, the icon-batch cache,
which is genuinely split across host and panel — `ensure_item_icon_srv()` is called from `draw_panel`
but `flush_item_icon_batch()` (same cluster) is called directly from `hk_present` right after the
draw calls, not from inside any of the 3 draw functions).

**Design decision (user, 2026-07-01):** keep `flush_item_icon_batch` host-side as-is; `draw_panel`
reaches the icon-batch cache through a ctx-held pointer instead of moving both functions together.

**Phase 1, slice 2 — DONE, build-verified + IN-GAME CONFIRMED (2026-07-01):** on reflection, most
of the audit's "panel-owned" cluster (`g_large`, the grace-debug-override family) turned out to
need NO ctx field at all — same treatment slice 1 gave `ensure_grace_srv`/`overlay_layers`: state
touched only by `draw_panel` and by helpers called only from the 3 draw functions collectively
isn't crossing the eventual host/draw-layer boundary, so it stays a file-static and moves bodily
with the code in Phase 2. Only the TRULY host-shared fields got added to `OverlayFrameCtx`:
`gamepad_combo_recording` (`bool*`, also R+W by `hk_present`'s XInput poll), `gamepad_combo_ready`
(`const bool*`, host-written only), `gamepad_combo_reject_reason` (`std::string*`, also written by
`hk_present`), `item_icon_srvs` (`std::map<int,ItemIconSrv>*`, also written by
`flush_item_icon_batch`). `atlas_srv`/`nav_frames` reused as-is from slice 1. `draw_panel` now
takes `const OverlayFrameCtx &ctx`; `hk_present` passes the same `frame_ctx` it already built.
Cross-build clean (clang-cl+xwin). Deployed to
`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, backup
`.bak-pre-panel-ctx`). **IN-GAME CONFIRMED 2026-07-01 20:47**: `[SIG]` 29/29 clean, no crash/error,
AND the user actually exercised the exact new ctx-plumbed path live — `[OVERLAY] Gamepad combo
rejected (single nav button): A` → `[OVERLAY] Gamepad combo recorded: Y+RB` → `[TOGGLEDIAG]
GAMEPAD toggle fired` (true then false) — confirms the riskiest host-shared cluster (recording/
ready/reject_reason round-tripping between `draw_panel` and `hk_present`'s XInput poll) works
correctly through the new pointer indirection. **Phase 1 is now COMPLETE for all 3 draw
functions** — not yet merged to `master`.

**Phase 2 — split into a reloadable module + host-side reload mechanism.**

**Two deploy modes (design decision, user, 2026-07-01):** the split is a DEV-ONLY convenience, not
a shipped architecture change — real players keep getting today's single `MapForGoblins.dll`.
  - **Release/single-DLL (default, unchanged):** `add_library(MapForGoblins SHARED ...)` as it
    exists today (`CMakeLists.txt:93`) — the draw-layer `.cpp`(s) compile straight into the one
    DLL, no `LoadLibrary`/vtable indirection at all. This is what `build.bat` and the current
    `build-linux` cross-build target produce; nothing here changes for them.
  - **Dev split (opt-in):** a second CMake target/option (`CMakeLists.txt` currently has NO
    `option()` calls at all — this introduces the first one, e.g.
    `option(GOBLIN_OVERLAY_HOTRELOAD "Build overlay draw layer as a separate hot-reloadable DLL" OFF)`)
    that, when ON, builds the draw-layer sources into `goblin_overlay_render.dll` instead and links
    the host DLL against a thin loader (`LoadLibrary`/`GetProcAddress`) rather than the object files
    directly. Default OFF so a plain `cmake -B build-linux ...` / `build.bat` keeps producing the
    single-DLL release shape; only an explicit `-DGOBLIN_OVERLAY_HOTRELOAD=ON` dev build produces
    the split pair for the Phase 3/4 AI iterate loop.
  - Both modes must build from the SAME source files (no forked copies) — the CMake option toggles
    which target the draw-layer `.cpp`s are added to, not which code exists.

Move the extracted draw layer into its own DLL (e.g. `goblin_overlay_render.dll`), loaded via
`LoadLibrary` from the host (hook) DLL. Host keeps `hk_present`/device ownership, calls through a
thin vtable/function-pointer table resolved via `GetProcAddress`. Dev-only file-watcher (mtime
poll on the render DLL, gated by a config flag) triggers `FreeLibrary` + rebuild + `LoadLibrary` +
rebind on change — no ERR restart. Key risks to solve here, not defer:

**Phase 2 ground-truth scoping audit (2026-07-01) — the boundary is bigger than "3 functions":**
confirmed self-contained (safe to move, only ever called from the 3 draw functions or each other):
`append_folded`/`fold_ci`/`contains_ci`/`matches_all_tokens` (item-search string matching),
`scale_control`/`draw_gamepad_keyboard_button` (panel widgets), `ensure_grace_srv`/
`ensure_grace_dungeon_srv`/`ensure_grace_debug`/`grace_candidate_gate_warning`/`grace_dbg_mapping`/
`force_rebuild_grace` (grace-sprite family), `overlay_layers()`, `page_label`, plus the panel-owned
statics already found in Phase 1 (`g_large`, `g_grace_dbg_*` family). **But**: `goblin::overlay::
native_item_icon`/`native_map_point_icon`/`native_map_point_icon_by_name`/`map_point_glyph_uv`
(`goblin_overlay.cpp:3890-3994`, declared `goblin_overlay.hpp:32-50`) are called from **`src/
worldmap/map_renderer.cpp`** (:153,157,185,209,224,569) — a SEPARATE translation unit, invoked
mid-frame from inside what `draw_worldmap_markers`/`draw_minimap_hud` call — AND directly from
`draw_panel` (:1847, census thumbnails). This whole native-icon-resolution subsystem (`SheetTex`/
`g_sheet_cache`/`copy_sheet_cached`, `MapSymSrv`/`g_map_sym_srv`/`ensure_map_sym_srv`, `DiskSheet`/
`g_disk_sheets`/`ensure_disk_sheet`/`request_disk_sheets`, `create_tex_from_dds_mem`) sits on the
SAME D3D12 render infra (`g_device`/`g_command_queue`/`g_srv_heap`) as Phase 1's icon-batch
cluster. **Consequence:** the real Phase-2 boundary is "the whole render pipeline," spanning
`goblin_overlay.cpp`'s draw functions AND `src/worldmap/*.cpp` (`map_renderer.cpp` confirmed;
`map_entry_layer.cpp`/`grace_layer.cpp`/`quest_npc_layer.cpp`/`category_meta.cpp` NOT YET AUDITED
for their own reverse dependencies back into host code — check before locking the file-move
boundary). Moving only the 3 functions and leaving `native_item_icon`/etc. host-side means every
marker draw crosses the DLL boundary anyway, defeating hot-reload's point if icon-resolution logic
needs iteration too (likely, given how much RE/tuning work already lives there).

**Follow-up audit (2026-07-01) — the 4 remaining `src/worldmap/*.cpp` files, resolved:** all 4 are
pure data-layer `MarkerLayer` subclasses — ZERO D3D12 (`ID3D12*`/`CreateCommittedResource`/
`CreateShaderResourceView`/`D3D12_GPU_DESCRIPTOR_HANDLE`: no hits in any of them) and ZERO calls
into `goblin::overlay::*` or any `goblin_overlay.cpp` D3D12 global. `map_entry_layer.cpp`/
`grace_layer.cpp`/`quest_npc_layer.cpp` each include `goblin_inject.hpp` for stable read-only
accessors (`marker_world_pos`/`category_visible`/`read_event_flag`/`goblin::live_graces`/
`section_visible`/`category_section`, etc.) — a MUCH easier cross-boundary shape than the D3D12
problem: these are ordinary functions, so they can cross via normal `dllexport`/`dllimport` (host
exports the accessors, render DLL imports them), no ctx-pointer/vtable scheme needed.
`category_meta.cpp` has ZERO reverse coupling of any kind (its only "host" include is a generated
baked-icon header, not `goblin_overlay.cpp`). **Net: the only genuinely hard cross-boundary problem
in all of Phase 2 remains the single `native_item_icon`/`native_map_point_icon`/
`native_map_point_icon_by_name`/`map_point_glyph_uv` family** (owns/mutates D3D12 resources, must
stay host-side, needs the reverse ctx/pointer table) — every other file in `src/worldmap/*.cpp`
either moves wholesale with zero coupling (`category_meta.cpp`) or just needs its
`goblin_inject.hpp` accessor calls declared `dllexport`/`dllimport` across the new boundary
(`map_entry_layer.cpp`/`grace_layer.cpp`/`quest_npc_layer.cpp`/`map_renderer.cpp`). Slice-B's
render-DLL source list is now fully scoped: `goblin_overlay_render.cpp` (new) + all 5
`src/worldmap/*.cpp` files.

**CMakeLists restructuring (no `option()` exists today — this introduces the first one):**
```cmake
option(GOBLIN_OVERLAY_HOTRELOAD "Build overlay draw layer as separate hot-reloadable DLL" OFF)
set(GOBLIN_RENDER_SOURCES
  src/goblin_overlay_render.cpp   # new: draw fns + grace/panel helpers
  src/worldmap/map_renderer.cpp src/worldmap/map_entry_layer.cpp
  src/worldmap/grace_layer.cpp src/worldmap/quest_npc_layer.cpp
  src/worldmap/category_meta.cpp)
set(GOBLIN_HOST_SOURCES <everything else, unchanged list minus GOBLIN_RENDER_SOURCES>)
if(GOBLIN_OVERLAY_HOTRELOAD)
  add_library(goblin_overlay_render SHARED ${GOBLIN_RENDER_SOURCES})
  add_library(MapForGoblins SHARED ${GOBLIN_HOST_SOURCES})
  target_link_libraries(MapForGoblins PRIVATE goblin_overlay_render)  # or LoadLibrary, no static link
else()
  add_library(MapForGoblins SHARED ${GOBLIN_HOST_SOURCES} ${GOBLIN_RENDER_SOURCES})  # today's shape
endif()
```
Both branches reference the SAME source files (no forked copies) — satisfies the two-deploy-mode
design decision above. `dllmain.cpp`'s `goblin::overlay::initialize()` (:292, installs the Present/
ResizeBuffers/ExecuteCommandLists MinHook hooks via `resolve_vtables`) stays host-side unchanged in
both modes; a `GOBLIN_OVERLAY_HOTRELOAD` build additionally needs it to `LoadLibrary`+
`GetProcAddress` the render DLL's vtable before returning.

**Recommended PR-slicing for Phase 2 (mirrors Phase 1 / goblin_inject_refactor_plan discipline):**
1. Slice A — CMake scaffold only (`option()` + source-list split), still ONE DLL either way, no
   `goblin_overlay_render.cpp` file yet, no `LoadLibrary`. Verifies the CMake restructuring is inert.
2. Slice B — physical file move (draw fns + private helpers + `map_renderer.cpp`, pending the
   4-file reverse-dependency audit above), still statically linked into one DLL when the option is
   OFF. Pure relocation, build+in-game confirm, same discipline as Phase 1's slices.
3. Slice C — actual DLL split + `LoadLibrary` boundary when `GOBLIN_OVERLAY_HOTRELOAD=ON`: vtable/
   function-pointer resolution, `native_item_icon`-family calls back into host D3D12 infra via a
   NEW reverse-direction ctx/pointer table (render DLL doesn't own `g_device` etc.).
4. Slice D — file-watcher + actual hot reload.
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
