#pragma once
#include <filesystem>

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
        // Snap-rect midpoint (WorldMapArea +0x340..+0x34c centre). The DEVICE-INDEPENDENT
        // view centre = (pan + snapMid)/zoom (engine pan setter FUN_1409cd100 inverted).
        // The reticle (+0xFC) is NOT the view centre — on gamepad it sits off-centre, which
        // is why reticle-centred markers are "jamais centré" until you move the mouse.
        float snapMidX, snapMidZ;
        float raw[8];           // diag: cursor+0xFC,+0x100,+0x104,+0x108,+0x10C,+0x110,+0x114,+0x118
        int viewArea;           // WorldMapArea+0x6e = areaNo of the open page (doc §3)
        int underground;        // open-map layer (dialog+0x2B68 deref +0xB8): 0=surface, 1=underground
        int openDlc;            // open-map page (dialog+0xA88): 1 if DLC map (page==10), else 0
    };
    bool get_live_view(LiveView &out);

    // DIAG: the currently-published active cursor address (0 = none). Lets the
    // overlay tell apart "probe hasn't found a cursor yet" (0) from "found but the
    // live read failed" (non-0 but get_live_view false) when chasing open latency.
    uintptr_t debug_active_cursor();
}
