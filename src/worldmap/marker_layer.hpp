#pragma once
// MarkerLayer — a generic source of map markers for the ImGui-rendered world map.
//
// This decouples DATA (what to show: graces, bosses, items, …) from DRAW (project +
// cull + render, owned by map_renderer). Each marker type is a MarkerLayer plugin;
// graces are the first impl (grace_layer). NOTE: this is the NEW overlay-rendered map,
// NOT the legacy native WorldMapPointParam injection in goblin_inject.
//
// Kept free of ImGui/Windows so it stays trivially testable; color is a packed ImU32
// (ABGR) so the interface needs no imgui include.

#include <cstdint>
#include <vector>

namespace goblin::worldmap
{
// A single marker in UNIFIED world space, pre-classified to a map group.
struct Marker
{
    float worldX = 0, worldZ = 0;
    // group = isDLC*2 | isUG : 0 base-overworld, 1 base-underground, 2 DLC-overworld,
    // 3 DLC-underground. The renderer draws only the OPEN group's markers.
    int group = 0;
    int srcArea = 0;            // original areaNo (diagnostics / converter choice)
    int category = -1;          // static_cast<int>(Category); drives the cluster opt-in
    int cluster_key = -1;       // nearest-grace group id (native by-location clustering); -1 = exact
    int loc_pname = -1;         // group's region PlaceName id (for the pile name label)
    int name_id = -1;           // this marker's own name textId (FMG lookup for the tooltip)
    unsigned int color = 0xEB82E65Au; // packed ImU32 ABGR — circle-fallback colour
    // Atlas cell key (goblin::overlay_icons ICON_CELLS, e.g. "show_graces"). When the
    // atlas is loaded and the key resolves, the marker draws as that icon; otherwise it
    // falls back to a coloured circle.
    const char *icon_key = nullptr;
    // Map-fragment discovery flag for this marker's tile (goblin::map_fragment_flag).
    // 0 = no fragment gate. When config::requireMapFragments is on, the renderer hides
    // the marker until this event flag is set (player found the area's map fragment).
    int fragment_flag = 0;
    // ── Collected/cleared graying (collected_graying config) ──────────────────────
    // All 0 = not applicable (e.g. graces) → never grayed. Set only by MapEntryLayer.
    uint64_t row_id = 0;     // original MAP_ENTRIES row id → rune/ember/kindling collected check
    int cleared_flag = 0;    // clearedEventFlagId: boss/NPC cleared → dim + green checkmark
    int collected_flag = 0;  // textDisableFlagId1: loot picked up → dim
    // Graces: their per-grace discovery flag (textDisableFlagId1). When SET (player has
    // discovered/rested there), the game draws that grace natively (generated from
    // BonfireWarpParam), so the renderer DROPS this overlay marker to avoid a double
    // icon. Undiscovered (flag unset) → drawn as the helper marker. 0 = no gate.
    int discover_flag = 0;
    // Post-event "secondary story flag" (legacy SetSecondaryFlags + Ashen Capital). When
    // SET this marker is a post-event variant (burnt/changed area) and only appears once
    // the story flag fires — the renderer HIDES it while the flag is unset (inverse of
    // discover_flag). 0 = no gate. Set only by MapEntryLayer.
    int secondary_flag = 0;
    // Post-event "hide" story flag — the INVERSE of secondary_flag. When SET this marker
    // is a PRE-event variant (e.g. Leyndell Royal Capital, area 11) that is replaced once
    // the story flag fires (the Ashen Capital takes over), so the renderer HIDES it the
    // moment the flag is set. 0 = no gate. Set only by MapEntryLayer. Mirrors the legacy
    // refresh_royal_eviction (which only ever ran on native injected rows).
    int hide_when_flag = 0;
    // Lot-backed loot marker (its item comes from an ItemLotParam row). Drives
    // anonymous_loot spoiler-free mode: when that config is on, this marker draws as a
    // gray "?" with a generic label instead of its real icon/name. Set only by
    // MapEntryLayer (false for pieces/kindling/bosses/graces — never lot-backed loot).
    bool lot_backed = false;
};

// A data source of markers. markers() returns the layer's cache (built lazily by the
// impl); the renderer iterates visible layers each frame.
struct MarkerLayer
{
    virtual ~MarkerLayer() = default;
    virtual const char *category() const = 0;       // display / toggle key
    virtual bool visible() const = 0;               // master + category gate (later)
    virtual const std::vector<Marker> &markers() const = 0;
};
} // namespace goblin::worldmap
