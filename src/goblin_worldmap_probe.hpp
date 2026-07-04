#pragma once
#include <filesystem>
#include <vector>

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)

// Read-only world-map CURSOR probe (proximity + live-refresh scouting, Linux/DLL).
//
// The Windows/Ghidra RE (docs/world_map_re_findings_windows.md) found the map
// cursor embedded in the world-map menu at menu+0x2DB0, with coords at +0xFC /
// +0x104 / +0x10C, in MAP/MARKER space (= the same space as WorldMapPointParam
// posX/posZ → proximity clustering needs NO chunk→world bridge if we use the
// cursor). This probe resolves the cursor instance by scanning process memory
// for the CS::WorldMapCursorControl vtable, then LOGS the three coords live so we
// can confirm the X/Z offset order + that they track the cursor in marker space.
//
// Also a PROJECTION transform-scan: follows cursor+0xF0 (CS::WorldMapArea) and logs
// the live viewport (pan +0x378, zoom +0x380) + the virtual UI canvas, so we can
// confirm the world→screen affine (docs/world_map_projection_re_findings.md §6).
//
// Opt-in (config debug_worldmap_probe). STRICTLY READ-ONLY + SEH-guarded → safe;
// the risky live-refresh call-test (FUN_140a832a0) is a separate future step.
namespace goblin::worldmap_probe
{
    void initialize(const std::filesystem::path &log_path);

    // Live projection inputs for the overlay-markers prototype. The active map
    // cursor's marker coord (+0xFC/+0x104) and its WorldMapArea (cursor+0xF0)
    // pan (+0x378) / zoom (+0x380). Read fresh from the cached active cursor each
    // call (read-only + SEH-guarded → safe on the render thread). Returns false
    // if the map is closed / no live cursor / a read faulted. Requires the probe
    // loop to be running (config debug_worldmap_probe OR overlay_markers_proto).
    struct LiveView
    {
        float cursorX, cursorZ; // reticle, marker space (+0xFC / +0x104)
        float panX, panZ, zoom; // WorldMapArea viewport
        // Cursor/snap bounds rect on the view (view+0x340..0x34c): minX,minZ,maxX,maxZ.
        // RE §2: viewCentre = (pan + ((min+max)/2)) / zoom -- the CURSOR-INDEPENDENT centre.
        float snapMinX, snapMinZ, snapMaxX, snapMaxZ;
        // Snap-rect midpoint (= (min+max)/2 of the rect above), precomputed by the probe.
        // The DEVICE-INDEPENDENT view centre = (pan + snapMid)/zoom (engine pan setter
        // FUN_1409cd100 inverted). The reticle (+0xFC) is NOT the view centre — on gamepad it
        // sits off-centre, which is why reticle-centred markers are "jamais centré" until you
        // move the mouse.
        float snapMidX, snapMidZ;
        // STATIC full-map rect (view+0x350..0x35c = [minX,minZ,maxX,maxZ], marker space) — the
        // engine's own pan-clamp bound = the extent of the map ART. The renderer projects it to
        // screen and clips marker draws to it (markers used to float on the letterbox void /
        // day-night dial past the map edge). All zeros when the read failed → renderer skips
        // the clip.
        float mapMinU, mapMinV, mapMaxU, mapMaxV;
        float raw[8];           // diag: cursor+0xFC,+0x100,+0x104,+0x108,+0x10C,+0x110,+0x114,+0x118
        int viewArea;           // WorldMapArea+0x6e = areaNo of the open page (doc §3)
        int underground;        // open-map layer (dialog+0x2B68 deref +0xB8): 0=surface, 1=underground
        int openDlc;            // open-map page (dialog+0xA88): 1 if DLC map (page==10), else 0
        // Page-transition state (RE: windows_worldmap_page_transition_re_findings.md, runtime-
        // CONFIRMED 2026-06-22). The page id (openDlc/underground) flips INSTANTLY at the button
        // press, but the VISUAL cross-fades over ~0.2s → the "flip too early" overlay flicker.
        // fadeTimer is the clean progress signal; the pan/zoom-target offsets from the doc were
        // wrong (and unneeded — marker positions already ride the live view, §4).
        int swapEdge;           // dialog+0xA44 (u8): 1 the frame a swap was requested (transient)
        float fadeTimer;        // dialog+0xE00 (f32): resets 0.2 at swap → ramps to 0 → settles
                                // ~-0.023; sign = layer (≥0 surface, <0 UG). progress=1-fade/0.2.
    };
    bool get_live_view(LiveView &out);

    // Pan the live world map so its view centres on a marker-space point (mU, mV) — same space as
    // project()'s output / WorldMapPointParam posX/posZ. Used by the F1 item-search "locate" to scroll
    // the map onto a clicked result (the real camera move, not a cursor nudge). Writes WorldMapArea
    // pan (+0x378/+0x37C); valid only for a point on the currently-open page. Returns false if the
    // map is closed / no live cursor / the write faulted. Map must be OPEN.
    // minZoom > 0: also ZOOM IN (write +0x380) if the view is currently more zoomed-OUT than minZoom,
    // so the located marker isn't lost in a tiny far-out view; never zooms OUT (respects a user who
    // zoomed in further). minZoom = 0 → pan only (legacy). Map zoom runs ~0.05 (whole map)..1.0.
    bool set_view_center(float mU, float mV, float minZoom = 0.f);

    // Diagnostic snapshot of the LAST set_view_center call — drives the F1 "Locate debug" overlay so
    // the edge-of-world "no centering" case can be compared live against a normal one. Written on the
    // render thread inside set_view_center, read by the overlay on the same thread.
    struct LocateDebug
    {
        bool ran = false;       // set_view_center was called at least once
        bool cursorOk = false;  // g_active_cursor live + view read OK (else everything below is stale)
        float reqU = 0, reqV = 0;        // requested centre (marker space, the clicked marker)
        float clampU = 0, clampV = 0;    // centre after the world-rect clamp
        bool clamped = false;            // clamp actually moved the centre (→ near an edge)
        bool rectOk = false;             // full-map rect (+0x350) read OK
        float wMinX = 0, wMinZ = 0, wMaxX = 0, wMaxZ = 0; // world rect used for the clamp
        float zoomBefore = 0, zoomUsed = 0;               // live zoom vs post zoom-in
        float snapMidX = 0, snapMidZ = 0;
        float panWroteX = 0, panWroteZ = 0;  // pan we computed + tried to write
        bool wrote = false;                  // both pan writes succeeded
    };
    const LocateDebug &last_locate_debug();

    // Request the live map to switch to a target PAGE GROUP (bit1 = DLC, bit0 = underground), so the
    // item-search locate can reach a cross-page result. The switch is marshalled onto the GAME thread
    // (executed inside the hooked per-frame map step FUN_1409c32f0) so it never races the UI — calling
    // the switch handlers (c1fc0 page / c7900 layer) from our render thread would corrupt the dialog.
    // Drains over a few frames (one axis per step). No-op if the marshal hook didn't install. The
    // persistent locate then pans the instant the page opens.
    // NOTE: does NOT yet gate on page availability — pinning page_selectable (don't switch to a page
    // the player hasn't unlocked) is the next RE pass; see windows_worldmap_page_switch_re_prompt.md.
    void request_switch_to_page(int group);

    // Item-search locate: drive the live map to CENTRE on a marker-space point. The per-frame map-step
    // hook (c32f0, game thread) writes this to the cursor reticle target each frame, so the engine's own
    // easer pans the view onto it and the write isn't reverted (RE §5b) — unlike a Present-thread pan
    // write, which the engine overwrites. Call every frame while the locate is held; clear when done.
    void set_locate_target(float u, float v);
    void clear_locate_target();
    // True while the engine is clamping the active locate target to the discovered-extent
    // bounds (deep-fog target). The overlay hold uses it to stop pumping the nav jitter so the
    // engine easer sleeps and the c32f0 detour's direct pan write sticks (F2 pan-OOB).
    bool locate_target_clamped();
    // Dev: hex-dump CSMenuMan + WorldMapDialog windows to the wmprobe log (RPC `dumpmenu <tag>`;
    // diff two tags to find the menu-over-map flag — HANDOFF z-order bug 2).
    void dump_menu_state(const char *tag);

    // True while a submenu is stacked OVER the open world map (fast-travel confirm, marker
    // dialog, region list, ...) — via CSMenuMan+0x104 (RE 2026-07-03, byte-diff). Reads 0 on
    // the bare map, incl. during pan/zoom. We render post-present, so render_markers skips the
    // whole worldmap marker pass while this is true (gated by clip_game_ui) — otherwise our
    // icons punch through the covering menu. Read-only + SEH-guarded → safe every frame.
    GOBLIN_RENDER_API bool menu_covers_map();

    // True while an auto page-switch is in progress OR still settling (the engine SNAPS the view to
    // the new page's default, which would clobber an item-locate pan issued too early). The locate
    // holds its pan until this is false so its set_view_center lands last and sticks.
    bool page_switch_busy();

    // TODO(page_og_underground_available): a clean per-page DIALOG availability flag is only HALF found
    // — DLC = dialog+0x27c8 (RPM-diffed, but read UNRELIABLY at runtime), underground = never located
    // (contextual). The overlay instead gates the auto-switch on GRACE DISCOVERY (no grace rested in a
    // region => never visited => don't teleport there), which is robust + covers both. If a clean
    // dialog flag is wanted later, finish 0x27c8 + find the UG one (page_full_dump.py diff tooling).

    // Live world→map-space projection: call the engine's own per-icon projection
    // (FUN_1408877d0 on the live CS::WorldMapViewModel) to map a raw (area, gridX,
    // gridZ, posX, posZ) to map-space UV — folds WorldMapLegacyConvParam + applies the
    // per-page affine exactly like the native map (RE: windows_world_to_mapspace_
    // projection_re_findings.md). Replaces the baked LEGACY_CONV + -7040/+16512 affine
    // + DLC eyeball. Returns false if the map is closed (no live VM) or the area isn't
    // placed by the game (e.g. m19 Chapel has no converter) → caller falls back / gates.
    // Self-resolves the VM from the active cursor (cached); direct in-process call (fast,
    // safe to batch at marker-build time). Map must be OPEN for the VM to exist.
    // page out: the live map PAGE the point lands on — 0 overworld, 1 base underground,
    // 10 DLC (the engine's page-table + area-12 override). Lets the overlay derive the
    // marker group without the baked LegacyConv fold: group = (page==10?2:0) |
    // ((area==12 || 40<=area<=43)?1:0).
    GOBLIN_RENDER_API bool project(int area, int gridX, int gridZ, float posX, float posZ, float &mapU,
                 float &mapV, int &page);

    // The live per-area converter affine (VM+0xF8 slot, findings §1) — the map-space transform the map ART
    // tiles must share so they align with markers (endgame phase-1a slice 3). Read LIVE from the active VM
    // (never hardcoded — see docs/plans/procedural_map_derivation_design.md), cached per area once the map
    // has been open. mapX = (worldX-originX)*scale + biasX ; mapZ = -(worldZ-originZ)*scale + biasZ.
    // Inverse (map→world, for tile placement): worldX = (mapX-biasX)/scale + originX ;
    //   worldZ = originZ - (mapZ-biasZ)/scale. area 60 = overworld. false if the map never opened.
    struct ConvAffine
    {
        int area = 0, gridXbase = 0, gridZbase = 0;
        float originX = 0, originZ = 0, biasX = 0, biasZ = 0, scale = 1.0f;
    };
    GOBLIN_RENDER_API bool get_converter_affine(int area, ConvAffine &out);
    void log_converter_slots();  // DIAG: dump all live converter slots (area/gridbase/origin) to the log

    // A live resident WorldMapTile's cell + map-space rect (position only; textures deferred). The engine
    // positioned it (region-walk applied), so mapU0/mapV0 → world via the proven fit aligns with markers.
    // dim 0=overworld/1=mid/2=coarse; gridX/gridZ from tileId (dim*10000+gridX*100+gridZ). See
    // windows_worldmap_tile_rect_reach_re_findings.md.
    struct ResidentTileRect { int dim, gridX, gridZ; float u0, v0, u1, v1; };
    // Walk the live tile trees off the open map and collect resident tiles' {dim,gridX,gridZ,rect}. Needs
    // the map OPEN (active cursor published). Returns the count. Read-only + SEH-guarded.
    GOBLIN_RENDER_API int harvest_resident_tiles(std::vector<ResidentTileRect> &out, int max_tiles = 2000);

    // DIAG: the currently-published active cursor address (0 = none). Lets the
    // overlay tell apart "probe hasn't found a cursor yet" (0) from "found but the
    // live read failed" (non-0 but get_live_view false) when chasing open latency.
    uintptr_t debug_active_cursor();

    // DIAG (mid-session resolution bug, RE 3ce2b18): walk ER's render-output list and
    // log each entry's ACTIVE float dims (+0x118/+0x11c) + dirty bit (+0x140) vs the
    // live backbuffer (bbW/bbH). After a mid-session resize the entry(ies) that stay
    // at the OLD resolution are the stale ones driving the 3D + map zoom. Read-only
    // (RPM-guarded). Gated by config debug_render_dims; call throttled.
    void dump_render_dims(float bbW, float bbH);

    // DIAG (overlay z-order Task B — native map CLIP rect): while the map is open, dump candidate
    // f32 rects from the live structs (virtual UI canvas, WorldMapDialog, WorldMapArea view,
    // WorldMapViewModel) as [MAPCLIP], ONE-SHOT per backbuffer resolution — re-dumping on a
    // resolution change is the discriminator (native screen scissor scales with bb; virtual-canvas
    // / marker-space rects don't). Read-only. Gated by config debug_map_clip_diag; render thread.
    void map_clip_diag(float bbW, float bbH);

    // DIAG (overlay z-order Task B2 — native map CLIP via D3D12 scissor). Sink for the
    // RSSetScissorRects detour (goblin_overlay.cpp): dedups (rect, map_open) and logs each distinct
    // pair once as [SCISSOR]. The map-layer scissor is a rect seen with mapopen=1 but never mapopen=0
    // (open/close the map, diff). Thread-safe (records on many threads). Gated by debug_scissor_probe.
    void note_map_scissor(int left, int top, int right, int bottom, bool map_open);

    // Companion to note_map_scissor for the RSSetViewports detour (D3D12_VIEWPORT floats). Logs each
    // distinct (viewport, map_open) once as [VIEWPORT]. Gated by debug_scissor_probe.
    void note_map_viewport(float x, float y, float w, float h, bool map_open);

    // No-restart fix for mid-session resolution / display-mode changes: edge-triggered
    // call to ER's complete swapchain re-apply (FUN_1419ed440 — release+ResizeBuffers+
    // recreate all render targets), fixing windowed/fullscreen/borderless. Render-thread
    // only; idempotent. Returns 1 if it fired. Gated by config fix_midsession_resolution.
    int reapply_render_res(int w, int h);

    // Called from hk_resize_buffers to flag that a swapchain resize happened, so the
    // hk_present enforcer fires the re-apply even when the dims read consistent (the
    // fullscreen-doubling case = stale GPU resources, unchanged dims). Self-suppresses
    // during the re-apply's own nested ResizeBuffers. Cheap (sets an atomic).
    void note_resize_event();
}
