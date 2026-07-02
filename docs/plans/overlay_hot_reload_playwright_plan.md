# Overlay hot-reload + AI Playwright loop (plan)

**Status:** Phases 1–3 COMPLETE and **IN-GAME VALIDATED (2026-07-02, on the LINUX box, ERR under
Proton — the "split-build validation is Windows-only" assumption was WRONG: LoadLibrary/
GetProcAddress/FreeLibrary and the TCP RPC all work under Wine).** Live session proof: split build
booted (`render module loaded ... gen0`), SIG 29/29 clean, disk-loot build (the never-before-live
Slice C loot_disk cross-DLL path incl. `register_runtime_entries`) ran crash-free, THREE live
reloads (RPC-forced gen1, watcher-auto gen2 + gen3 — watcher swap landed **1.3s after the copy**),
and the full dev loop closed end to end: edited the F1 panel title in `goblin_overlay_render.cpp`
→ rebuilt ONLY the render target → auto-swap → RPC screenshot shows "Map for Goblins
[HOT-RELOADED gen-test]" in the running game, 60 fps, game never restarted. Phase 3 RPC commands
all validated live (ping/status/open_f1/set/screenshot/reload_overlay incl. error paths).
**Gotcha found on the way:** the FIRST split-build launch crashed (AV in the render DLL during the
disk build) — built from the STALE `build-linux-hotreload` cache configured before the toolchain
gained `/Z7 /Brepro`; a fresh reconfigure (needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under CMake
4) built a crash-free binary that survived boot + disk build + 3 reloads. Root cause not fully
pinned (stale flags vs. heisenbug) — if it EVER recurs on a fresh build, the PDB pair now ships to
the deploy dir for symbolized triage. Remaining: Phase 4 (the actual AI iterate loop) + optional
real-Windows spot-check. Raised by <user> 2026-07-01: reload ONLY the ImGui overlay
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
1. **Slice A — DONE, build-verified (2026-07-01, `feat/overlay-hotreload-cmake-scaffold`).**
   Added `GOBLIN_OVERLAY_HOTRELOAD` option (first `option()` in `CMakeLists.txt`) +
   `GOBLIN_RENDER_SOURCES`/`GOBLIN_HOST_SOURCES` list split (render list = the 5
   `src/worldmap/*.cpp` files per both audits above; `goblin_overlay_render.cpp` doesn't exist yet
   so `goblin_overlay.cpp` itself stays in `GOBLIN_HOST_SOURCES` until Slice B). Both variables
   still feed the ONE `MapForGoblins` target regardless of the option value — `if(GOBLIN_OVERLAY_HOTRELOAD)`
   only emits a `message(WARNING ...)` noting Slice B/C aren't landed. Cross-build clean both
   `OFF` (default) and `ON` (confirmed the warning fires, confirmed the combined source list still
   links). Deployed the `OFF` build (zero `.cpp` content changed, so functionally identical to the
   already-in-game-confirmed panel-ctx deploy — CMake reorg only, no runtime risk).
   **IN-GAME CONFIRMED 2026-07-01 21:07**: fresh session, `[SIG]` 29/29 clean, no crash/error.
   **MERGED to `master`** (fast-forward, branch deleted).
2. **Slice B — DONE, build-verified + IN-GAME CONFIRMED (2026-07-01, `feat/overlay-render-split`,
   not yet merged).** Moved `draw_panel`/`draw_worldmap_markers`/`draw_minimap_hud` + their
   genuinely self-contained helpers (item-search string matching, `overlay_layers`,
   `scale_control`, `draw_gamepad_keyboard_button`, `grace_candidate_gate_warning`, `g_large`) into
   a new TU, `src/goblin_overlay_render.{cpp,hpp}`. **Correction found mid-implementation**: the
   grace-SRV/icon-SRV helpers (`ensure_grace_srv`, `ensure_grace_dungeon_srv`, `force_rebuild_grace`,
   `ensure_item_icon_srv`, `ensure_grace_debug`, `copy_er_sheet_direct`, `create_tex_from_dds_mem`)
   were WRONGLY assumed self-contained by earlier audits — they directly touch host-owned
   `g_device`/`g_command_queue`/`g_srv_heap`/`g_frames`/`g_command_list`, the same per-frame D3D12
   state `hk_present` resets every frame, so moving them breaks (they'd lose access to those
   file-statics across the new TU boundary). Kept them in `goblin_overlay.cpp` behind ~15 thin
   forwarding wrappers/getters (`grace_state`/`grace_dbg_fmt_used`/`grace_dbg_srgb_ptr`/
   `grace_dbg_swiz_ptr`/`grace_srv_info`/`grace_dungeon_srv_info`/`grace_debug_candidates` + direct
   forwards for the 7 D3D12 functions) declared in `goblin_overlay_render.hpp`. `worldmap/*.cpp`
   files NOT moved this slice (already correctly bucketed into `GOBLIN_RENDER_SOURCES` since Slice
   A, no file move needed — the "5 worldmap files" language in the CMakeLists comment refers to
   that existing assignment, not a pending move). Still ONE binary — `GOBLIN_OVERLAY_HOTRELOAD` has
   no runtime effect yet. Cross-build clean. **IN-GAME CONFIRMED 2026-07-01 21:37**: both grace
   SRVs built successfully through the new getter/wrapper chain (`[GRACE-SRV] copied ...` +
   `[GRACE-SRV] DUNGEON copied ...`), `render.minimap` bench firing the whole session, `[SIG]` 29/29
   clean, no crash/error — validates the riskiest part of this slice (the grace-sprite cross-TU
   plumbing) live. Not yet merged to `master`.
3. **Slice C — actual DLL split + `LoadLibrary` boundary when `GOBLIN_OVERLAY_HOTRELOAD=ON`.**
   Export-surface audit (2026-07-01) found the real cross-DLL surface is ~111 call sites, bigger
   than the earlier "`native_item_icon` + 3 accessors" estimate — spans `goblin::config` (64
   globals), `goblin::ui` (29 fns, host in `goblin_section_visibility.cpp`), `goblin::worldmap_probe`
   (9 fns), 52 bare `goblin::*` fns (spread across `goblin_inject.cpp` + its PR-split files),
   `goblin::markers`/`kindling`/`collected`/`debug_events`/`sig`/`input` (a dozen more), plus
   `worldmap::disk_loot_dir`/`disk_loot_state` (host, despite the `worldmap::` name —
   `loot_disk.cpp` is in `GOBLIN_HOST_SOURCES`, not the render group). NOT cross-boundary:
   `goblin::projection::*` (all `inline`, compiles into both DLLs free) and most `goblin::worldmap::*`
   (already defined in the render-side files themselves). **Design decision (user, 2026-07-01):**
   consolidated wrapper API — one new file exposing thin forwards/getters, same mechanical pattern
   Slice B validated for the grace/icon-SRV helpers — rather than annotating the ~15 existing host
   headers directly, to keep blast radius contained to new files only. The 64 `goblin::config::*`
   globals (all `extern bool/int/float`, mostly simple, some bound as mutable `int*`/`bool*` to
   ImGui widgets and needing pointer-getters not value-getters) are near-identical in shape — an
   X-macro list is the planned mechanism instead of hand-writing 64 near-identical functions.
   **Open-items resolved (2026-07-01):** exact 59/64 config classification done by grepping
   `&goblin::config::X` (address-of/mutable) and `X = ` (direct-assignment) patterns across both
   render files — 45 mutable (pointer-getter), 19 pure read-only (still given pointer-getters
   anyway for a single uniform shape), plus `showCategory` (array, pointer-getter) and
   `questProgress`/`regionToggles` (`std::string`, reference-getters); the other 5
   (`lootFromDiskMsb`/`lootCollectibles`/`lootEnemyDrops`/`lootEmevdDrops`/`worldFeaturesFromDisk`)
   turned out `inline constexpr` — already free, no export needed. `GraceCandidate`/`LiveGrace`
   (`goblin_inject.hpp`), `RuntimeEntry` (`goblin_collected.hpp`), `SigHealth`/`sig_health()`
   (`re_signatures.hpp`, itself `inline` — free, no wrapper), `LiveView`/`LocateDebug`
   (`goblin_worldmap_probe.hpp`), `NpcQuest` (`generated/goblin_quest_steps.hpp`) — all confirmed
   in headers both sides already include, free by construction, only the FUNCTIONS operating on
   them needed wrappers. `flag` resolved to `goblin::flag::*` — `constexpr int` event-flag IDs in
   `goblin/goblin_map_flags.hpp`, free. `overlay_icons` resolved to `goblin::overlay_icons`, the
   generated baked-icon-atlas namespace, free (data tables, not functions).
   `goblin::marker_group_from` turned out `inline` too — free, no wrapper (found only once actually
   forwarding it and hitting a redundant-wrapper realization).

   **API layer DONE, build-verified standalone (2026-07-01, `feat/overlay-render-api`, not yet
   wired in, not yet merged).** New `src/goblin_overlay_render_api.{hpp,cpp}` — ~110 forwarding
   functions/getters, one consolidated file per the design decision above. Getting exact signatures
   right took several grep→compile-error→fix passes: multiple bare-`goblin::*` functions
   (`marker_world_pos`, `marker_fragment_flag`, `marker_cluster_key`, `resolve_loot_flag`,
   `resolve_loot_item_textid`, `lot_row_in_table`, `lot_item_count`, `diag_loot_flags`,
   `npc_loot_lot`) have real signatures FAR more complex than their names suggest (5-9 params,
   `uint8_t`/`uint32_t` area/grid/lot-type encodings, out-params) — audit-derived names were right,
   guessed signatures mostly weren't; `quest_step_done` doesn't exist under `goblin::` at all (it's
   `goblin::quest_step_done(const generated::NpcQuest&, size_t)` from a DIFFERENT header,
   `goblin_quest_steps.hpp`, not `goblin_inject.hpp`); `gpu_want_symbol`/`gpu_want_item` return
   `void` not `bool`; `goblin::markers::category_name`/`goblin::kindling::is_row_collected`+
   `region_row_id`/`goblin::debug_events::arm_capture`+`capture_armed`+`capture_count`+
   `finalize_capture` (the last takes a raw `bool(*)(uint32_t)` function pointer — forwards fine,
   plain C function pointers cross DLL boundaries without issue) were ALL wrong initial guesses
   (audit gave approximate names only, not real ones) — general lesson: for this remaining class of
   work, read the real declaration before writing a forward, don't infer from a name alone. This
   layer is currently dead code (not called from anywhere) — zero runtime risk, verified by
   standalone compile only, no in-game check needed until it's actually wired in.

   **Call-site rewiring — DONE, build-verified + IN-GAME CONFIRMED (2026-07-01,
   `feat/overlay-render-api-wired`, not yet merged).** All 6 render-side files now call
   `goblin::overlay_api::*` instead of `goblin::config::*`/`goblin::ui::*`/etc. directly, generated
   via a ~180-rule sed script (per-symbol, not blanket-namespace, to avoid rewriting TYPE references
   that must stay untouched): config address-of sites → `cfg_X_ptr()`, bare reads →
   `(*cfg_X_ptr())`, `showCategory`/`questProgress`/`regionToggles` via their dedicated getters; the
   5 `inline constexpr` config globals, `goblin::overlay::*` (the 3 draw functions + `native_item_icon`
   family), and render-internal `goblin::worldmap::*` all correctly left untouched. Found ONE
   export-audit gap while rewiring: `goblin::ui::section_label` was missing from the API entirely
   (added). Cross-build clean. **IN-GAME CONFIRMED 2026-07-01 22:12**: `[SIG]` 29/29 clean, both
   grace SRVs built, `render.minimap`/`refresh.collected`/`refresh.category_census`/
   `refresh.flag_or_pairs` all firing correctly the whole session — confirms config, `ui::`, and
   `collected::` wrappers all work live, no crash/error. Not yet merged.

   **`native_item_icon` reverse wrapping — DONE, IN-GAME CONFIRMED, MERGED (2026-07-01,
   `feat/overlay-native-icon-wrap`).** Turned out to need no new mechanism — same D3D12-coupled-
   host-function pattern as the grace/icon-SRV wrappers already established (these own the
   SheetTex/MapSymSrv/DiskSheet caches). Added `native_item_icon`/`native_map_point_icon`/
   `native_map_point_icon_by_name`/`map_point_glyph_uv` to the consolidated wrapper API, rewired
   all 9 call sites in `goblin_overlay_render.cpp`/`map_renderer.cpp`. In-game: `render.worldmap.markers`
   fired 574+ times drawing world-map icons through the new path, normal timings, `[SIG]` 29/29, no
   crash/error.

   **LoadLibrary mechanism — design locked (2026-07-01), not yet implemented.** This piece does
   ONLY a one-time load-and-resolve (proves the two-DLL split works end to end); the live
   FreeLibrary/reload/rebind cycle is Slice D's job, not this one.
   - **Render → host (already works, unchanged):** normal `dllexport`/`dllimport` on the ~110+
     `goblin_overlay_render_api.hpp` declarations via a new macro (`GOBLIN_RENDER_API`) in a tiny
     new header, active only when `GOBLIN_OVERLAY_HOTRELOAD_BUILD` is defined:
     `#if defined(MapForGoblins_EXPORTS) dllexport #else dllimport #endif` — CMake already auto-
     defines `MapForGoblins_EXPORTS` only when compiling that target's sources (confirmed in the
     build log), so this needs zero new CMake wiring beyond passing `GOBLIN_OVERLAY_HOTRELOAD_BUILD`
     to both targets. No-op (empty macro) when the option is OFF, so the default single-DLL build
     is untouched.
   - **Host → render (new): NOT normal linking.** The 3 draw functions get `extern "C"` trampolines
     with stable names in `goblin_overlay_render.cpp` (guarded by `GOBLIN_OVERLAY_HOTRELOAD_BUILD`),
     e.g. `extern "C" __declspec(dllexport) void MFG_DrawPanel(const OverlayFrameCtx *ctx)` forwarding
     to `goblin::overlay::draw_panel(*ctx)` — deliberately NOT dllimport/dllexport C++ linkage,
     because Slice D needs to `GetProcAddress` these by name after a `FreeLibrary`+reload, which
     load-time-bound imports can't do.
   - **ImGui context sharing:** `OverlayFrameCtx` gets a new `ImGuiContext *imgui_ctx` field (host
     sets it once, from the existing `ImGui::CreateContext()` call in `init_imgui`); each `extern
     "C"` trampoline calls `ImGui::SetCurrentContext(ctx->imgui_ctx)` before the real draw call —
     both DLLs statically link their own copy of the `imgui` library (separate global state per
     DLL by default), so this is required, not optional, the moment there are two binaries.
   - **Host loader:** new `goblin::overlay::call_draw_panel/call_draw_worldmap_markers/
     call_draw_minimap_hud` (host-side, e.g. folded into `goblin_overlay.cpp`) — `hk_present`'s call
     sites stay EXACTLY as they are today; these 3 functions internally branch on
     `GOBLIN_OVERLAY_HOTRELOAD_BUILD` (function-pointer call in the split build, direct
     `goblin::overlay::draw_*` call in the default build) — no visible change at the call site
     either way. `LoadLibraryW` resolves `goblin_overlay_render.dll` from the SAME DIRECTORY as the
     host DLL (`GetModuleFileNameW` + strip filename), not default search order. One-time load in
     `goblin::overlay::initialize()`; failure sets `g_failed = true` same as every other init
     failure path (mod disables gracefully, doesn't crash).
   - **CMakeLists (`GOBLIN_OVERLAY_HOTRELOAD=ON` only):** real two-target split —
     `add_library(goblin_overlay_render SHARED ${GOBLIN_RENDER_SOURCES})` linked against
     `MapForGoblins` (its import lib, for the ~110 `overlay_api` calls — standard load-time
     dependency, resolved automatically since the OS already loaded `MapForGoblins.dll` before
     `LoadLibrary`-ing render) + `imgui`/`minhook`/etc; `MapForGoblins` does NOT link
     `goblin_overlay_render` (runtime-loaded only). `add_dependencies(goblin_overlay_render
     MapForGoblins)` so the host (and its import `.lib`) builds first. Default `OFF` path
     (today's single `add_library(MapForGoblins SHARED ${GOBLIN_HOST_SOURCES} ${GOBLIN_RENDER_SOURCES})`)
     stays byte-for-byte unchanged.
   - **Not this slice:** live reload (`FreeLibrary`+rebuild+`LoadLibrary`+rebind), the file-watcher,
     and the threading/lock discipline that only matters once reload is real — all Slice D.

   **IMPLEMENTED + build-verified end to end (2026-07-01, `feat/overlay-loadlibrary-mechanism`).**
   Both `build-linux` (default OFF) and `build-linux-hotreload` (`GOBLIN_OVERLAY_HOTRELOAD=ON`,
   real two-DLL split) link and build clean. New files: `src/goblin_dll_export.hpp`
   (`GOBLIN_RENDER_API` macro), `src/goblin_overlay_render_loader.{hpp,cpp}` (consolidates ALL
   host→render calls — not just the 3 draw functions; the real link surfaced 3 more:
   `prebuild_markers`/`inworld_hovered`/`refresh_overlay_census`, called from
   `dllmain.cpp`/`input_wndproc.cpp`/`goblin_section_visibility.cpp` — same extern "C"+
   GetProcAddress treatment). `load()` runs once early in `dllmain.cpp`'s init sequence (BEFORE
   `prebuild_markers` needs it, not inside `goblin::overlay::initialize()` which runs later) and is
   idempotent so `initialize()` can also call it for the `g_failed` gate specifically.

   **Two real design corrections found only by the actual link, not by auditing:**
   1. **Render calls MORE host functions than either audit found** — `loot_disk.cpp`'s disk-
      treasure/quest-npc loaders (`load_disk_treasures`, `load_lod_treasures`, `load_quest_npcs`,
      `load_emevd_world_feature_flags`, `load_lod_feature_assets`, `load_emevd_awards`,
      `load_lod_award_entities`, `disk_source_enabled`, `ensure_map_dir_resolved`,
      `set_build_trigger`), `goblin_worldmap_probe::project()`, and `goblin::ui::read_event_flag`
      (called directly by a GENERATED file, `goblin_quest_steps.cpp` — can't be hand-edited to use
      a wrapper since it's regenerated).
   2. **Raw `extern` DATA can't be rescued by a wrapper function** — `goblin::config::*` globals
      and `from::params::param_list_address` are plain `extern` variables; `dllexport`/`dllimport`
      must be on the declaration the DEFINING `.cpp` sees (i.e. what `goblin_config.cpp`/
      `params.cpp` themselves include), so a SEPARATE getter-function wrapper in
      `goblin_overlay_render_api.cpp` doesn't help — the variable's OWN declaring header needs the
      macro. Fixed by directly annotating `goblin_config.hpp` (all 86 `extern` declarations, one
      `sed` pass), `goblin_inject.hpp` (just `read_event_flag`, for the generated-file case above),
      `goblin_worldmap_probe.hpp` (`project`), `from/params.hpp` (`param_list_address`), and
      `loot_disk.hpp` (the 10 loader functions) directly with `GOBLIN_RENDER_API`, abandoning the
      "touch zero existing headers" purity for these specific cases — proved more robust AND less
      work than chasing every call site (including ones reached via macros like `GOBLIN_BENCH`, or
      generated files that can't be edited at all).
   3. **A few pure-data generated files needed duplicate compilation into the render target**
      (`goblin_quest_steps.cpp`, `goblin_world_feature_models.cpp`, `goblin_map_data.cpp`,
      `goblin_name_regions.cpp`, `goblin_major_regions.cpp`, `generated_shared/goblin_overlay_icons.cpp`)
      — safe since they're const data tables with no mutable shared state; each DLL just gets its
      own copy.

   **General lesson for Slice D:** link-time verification (build BOTH configs for real) is the
   ONLY reliable way to find the true cross-DLL surface — every audit pass this plan did
   (multiple rounds) still missed real gaps that only the actual linker caught. Budget for this
   when scoping Slice D's own work.

   **Not yet done:** in-game confirm of `build-linux-hotreload`'s actual runtime behavior (this is
   Windows-only work — `LoadLibrary`/the real render DLL load — and this box is Linux-only;
   the default single-DLL build IS in-game confirmed, proving the macro plumbing is inert there).
   Deploying + confirming the split build live is a natural next step before or during Slice D.
4. Slice D — file-watcher + actual hot reload. **IMPLEMENTED + build-verified (2026-07-02,
   `feat/overlay-hotreload-slice-d`; both configs link clean, exports verified via the DLLs' export
   strings). Windows in-game validation still open.** What the pre-implementation audit found and
   how each risk landed:
   - **/MT cross-heap corruption (the biggest finding — a LATENT SLICE C BUG, not just a Slice D
     concern).** `/MT` (required for injection) gives each DLL its own CRT heap, and ~8
     `overlay_api` functions pass `std::string`/`std::vector` across the boundary by value, by
     move, or via out-param (`lookup_text_utf8`, `lookup_name_en_disk_utf8`, `mask_to_combo_string`,
     `harvested_ids`, `grace_candidates`, `tpf_dds_at`'s out-vector,
     `register_runtime_entries(std::move(...))`, `cfg_questProgress_ref`/`cfg_regionToggles_ref`
     reallocation) — allocate on one heap, free on the other. Fixed WHOLESALE, not per-signature:
     `src/goblin_render_new_override.cpp` (render-target-only source) overrides the render DLL's
     global `operator new/delete` (all variants incl. aligned/nothrow/sized) to forward to host
     exports `MFG_HostAlloc/MFG_HostFree(Aligned)` (defined in `goblin_overlay_render_loader.cpp`)
     — ONE heap for every C++ allocation on both sides, the entire api surface safe by
     construction, including allocations that outlive a reload. Audited-safe without changes:
     `reject_reason->clear()` (no dealloc), `item_icon_srvs` (render read-only), `live_graces()`
     (const ref).
   - **ImGui allocations are NOT covered by the operator-new override** (imgui allocates via malloc
     wrappers behind per-DLL `GImAllocator*` statics): render-side draws grow buffers inside the
     HOST-owned context that the host later frees. Fixed: `OverlayFrameCtx` carries the host's
     allocator triple (`ImGui::GetAllocatorFunctions`, filled per-frame in `hk_present`), and each
     draw trampoline applies it once per module load via `apply_imgui_bindings()` (a per-module
     static that naturally resets on every reload) before `SetCurrentContext`.
   - **The detached disk-build worker** (`map_entry_layer.cpp`'s `start_build_worker`, ~0.7s of
     render-DLL code on a thread the loader can't see) is THE FreeLibrary hazard. Three-layer fix:
     (1) new render export `MFG_RenderIdle` (reads `g_disk_running`) gates the swap — not idle →
     retry next frame; (2) re-checked under the exclusive lock (a `call_*` could slip in between
     check and acquire); (3) the old module's `FreeLibrary` is DEFERRED by one reload
     (`g_prev_module`) because the worker stores `g_disk_running=false` a few instructions before
     actually leaving render code — identity-swap-now, free-later closes that tail race.
   - **Host-held render function pointer** (`loot_disk`'s `g_build_trigger` →
     `kick_disk_build`, the ONLY render→host callback registration, found by audit): nulled via
     `set_build_trigger(nullptr)` before the swap, re-registered by the NEW module's
     `prebuild_markers()` after it. (Tiny benign TOCTOU at the `if (g_build_trigger)` call site;
     dir discovery is a one-shot early event, long past by any dev-reload time.)
   - **Threading/locking:** all host→render calls already funnel through the loader's `call_*`
     chokepoints → SRW lock, shared in every `call_*` (with a `thread_local` depth guard — SRW is
     non-reentrant), exclusive only during the swap. The swap itself runs at the TOP of
     `hk_present` between frames (present thread = the only draw-call thread, so no draw is ever
     mid-flight); `inworld_hovered` (wndproc thread) and `refresh_overlay_census`
     (section-visibility watcher thread) are fenced by the shared lock. Inside the reload, raw fn
     pointers are used on purpose (a `call_*` would shared-acquire under the held exclusive =
     self-deadlock).
   - **File locking (why copies):** Windows locks a loaded module's file, so loading the built
     `goblin_overlay_render.dll` directly would make the very FIRST rebuild fail to link. `load()`
     and every reload copy it to `goblin_overlay_render.hot<gen>.dll` first and load the copy
     (per-generation names — the current copy is itself locked); stale copies are deleted
     best-effort at init. The watcher (500ms host-side poll thread) flags a reload only once the
     source file's mtime+size differ from the loaded generation, are STABLE across two polls, AND
     the file opens with sharing denied (linker done writing).
   - **Fresh-statics re-init:** a reloaded module starts with empty marker buckets and an
     unregistered build trigger — `maybe_reload()` ends by calling the new module's
     `prebuild_markers()` (re-registers the trigger + kicks the rebuild) and
     `refresh_overlay_census()`. Reload failure keeps the OLD module live (trigger re-registered,
     pending flag cleared — the next successful rebuild re-arms via its new mtime).
   - **D3D12 handle lifetime:** as predicted safe by construction — grep audit found no D3D12
     handle captured as a render-side static (the SRV/sheet caches all live host-side per Slice B/C).
   - **In-game validation (open, Windows-only):** deploy the split build, confirm Slice C's
     one-time load, then the full live cycle: edit render source → rebuild ONLY
     `goblin_overlay_render` → watch `[HOTRELOAD] render gen<N> live` → markers/panel redraw
     correctly, no heap corruption over repeated reloads (the /MT fixes above are exactly what a
     several-reload soak would stress).

**Phase 3 — Route B debug RPC. IMPLEMENTED + build-verified (2026-07-02,
`feat/overlay-debug-rpc`); in-game validation open.**
- **Transport: TCP loopback, NOT a named pipe — deliberate change from the original sketch.** The
  live game runs under Proton on the Linux box, and Wine's loopback IS the host's loopback: a
  Python driver on Linux reaches 127.0.0.1 straight into the Wine process, which a Windows named
  pipe can't do. Same code works on real Windows. This makes the whole Phase 3/4 loop drivable
  from the Linux box.
- **Gate:** ini `[Debug] debug_rpc_port` (String; empty = disabled = default; loopback-only bind,
  no auth). `goblin::debug_rpc::initialize()` (dllmain, after `overlay::initialize`) starts the
  listener thread; bind failure just logs + disables.
- **Threading model:** the listener thread ONLY parses lines and queues them
  (`shared_ptr<Pending>` + event per command — no use-after-free even if the listener times out
  while the present thread still executes). Every command EXECUTES on the present thread:
  `debug_rpc::pump(swapchain)` at the END of `hk_present`, after the overlay's
  `ExecuteCommandLists` — so a `screenshot` copy is queue-ordered behind the overlay draw and the
  capture INCLUDES the overlay. Idle cost ≈ one relaxed atomic read per frame. 10s command
  timeout (game not presenting) → `err timeout`.
- **Commands (line protocol, reply `ok ...`/`err ...`):** `ping`; `status` (panel/hotreload/gen/
  reload_pending — poll `gen` to watch a reload land); `open_f1 0|1|toggle` (flips the same
  `g_user_show` the F1 key does); `set <ini_key> <value>` — GENERIC config setter through the ini
  schema (new `goblin::config_set_by_key`, same typed parser as load_config, runtime-only, no
  persist; ERR-only keys rejected off-ERR) — covers the planned `set_scale`/`toggle_cluster` and
  every other key without per-key code (caveat: content-affecting keys may need their usual
  refresh path); `screenshot <path>` — backbuffer → 24-bit BMP
  (`overlay::screenshot_to_file`: readback buffer + `GetCopyableFootprints`, PRESENT→COPY_SOURCE
  barriers, fence wait, BGRA/RGBA formats; BMP = zero new dependencies, PIL reads it); and
  `reload_overlay` — flags the Slice D swap exactly like the file watcher
  (`overlay_render_loader::request_reload`; `err not a hotreload build` in the default config).
- **NOT implemented: `search "<item>"`** — the F1 item-search buffer (`item_q`) is a render-side
  static; poking it from the host needs a new cross-DLL export. Followup when Phase 4 actually
  needs it (drive the UI via `set` + screenshots meanwhile).
- **Driver: `tools/mfg_rpc.py`** — `Rpc` class (`cmd`/`status`/`wait_reload`) + CLI; protocol
  smoke-tested against a fake server (multi-arg commands, status parsing, exit codes). NB
  screenshot paths are interpreted by the GAME process — under Proton pass a Wine path
  (`Z:\tmp\shot.bmp`).
- **Latency, measured live (2026-07-02, title screen @60fps):** every command ≈ ONE frame — ping/
  status/set all avg 16.66ms (min ~15, p95 ~17, tight jitter), by design: commands queue and
  execute at the next `hk_present`; TCP loopback and the execution itself are noise. Screenshot
  avg 20.5ms (GPU readback + 6MB BMP write adds only ~4ms). Throughput is 1 command/frame
  (burst of 50 pings = 16.45ms/cmd) because `serve_client` waits for each reply before reading
  the next buffered line — if a future driver needs >60 cmd/s, pipeline server-side (queue all
  buffered lines per frame, reply in order); not worth it for the iterate loop.
- **In-game validation (open, doable on THIS box under Proton):** set `debug_rpc_port = 38700`,
  launch ERR, then `tools/mfg_rpc.py --port 38700 ping/status/screenshot ...`; `reload_overlay`
  additionally needs the split build deployed.

**Phase 4 — wire the AI iterate loop. FIRST REAL RUN COMPLETE (2026-07-02,
`feat/phase4-device-aware-hints`).**
Target picked per the ground-truth rule: backlog item 2's open UI half (device-aware close-hint —
the F1 panel said "F1 close" even for gamepad users; now shows the configured combo, e.g. "Y+R3
close", when `last_input_was_gamepad()`). The loop as actually run: screenshot baseline ("F1
close") → implement → rebuild ONLY render → watcher auto-swap → screenshot verify → **hot-reload a
condition-FORCED build to visually verify the gamepad branch without owning a gamepad** ("Y+R3
close" confirmed on-screen) → revert → reload → final screenshot. Three live iterations,
~15s each edit-to-verified-pixels, game never restarted. Loop constraint learned: without a
loaded save, only title-screen + F1-panel targets are verifiable (no game-input injection in the
RPC yet — a `load save / press key` command is the natural next unblocker for worldmap-marker
targets like F2/spiderfy).

**Phase 4 prerequisite bug found + fixed on the way (the SECOND split-build boot crash):** the
"stale cache" theory from the Phase 3 session was WRONG — a fresh build crashed identically (AV in
the render module during boot, PDB-symbolized this time: `spdlog::logger::should_log`, garbage
default-logger pointer). Root cause: each /MT DLL has its own spdlog registry, so render-side
`spdlog::` calls lazily created render's OWN default logger inside hooked paths on whichever
thread logged first — an intermittent first-touch race, AND every render-side log line
([LANDMARKLIVE], [LOOTDISK]-build, render [BENCH]) silently vanished from MapForGoblins.log in the
split build (confirmed: 0 occurrences in the Phase 3 session log). Fix:
`src/goblin_render_log_bridge.cpp` (render-only) — `MFG_RenderInitLogging` installs a default
logger whose sink forwards each line to the host's `MFG_HostLogLine` export; the loader calls it
as the FIRST render call after GetProcAddress (deterministic, single-threaded). Validated: 300
render lines in the host log, crash-free boot, 2 further reloads clean.

## Non-goals

- Does not touch input hooks (`src/input/*`) — those stay host-side, no reload need identified.
- Does not replace Route A (offline mock-data harness) — that's for layout/clutter iteration with
  zero game dependency and can land on its own schedule.
- Not cross-platform: LoadLibrary/FreeLibrary + MinHook is Windows-only; build/test this on the
  Windows dev box per `AGENTS.md`'s platform rule.
