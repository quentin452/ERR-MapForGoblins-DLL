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

#include <vector>

namespace goblin::worldmap
{
// Map group for the Chapel of Anticipation (m19, the prologue). Out of the renderer's
// open_grp range (0..3 = OW/UG/DLC × surface/underground), so these markers are KEPT in
// the data but never drawn by the current renderer (no leak onto the overworld) — ready
// for a future m19 map page once its world→map-space converter is dumped (TODO/RE). Until
// then area-19 rows have no valid Lands-Between position.
constexpr int GROUP_CHAPEL = 4;

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
