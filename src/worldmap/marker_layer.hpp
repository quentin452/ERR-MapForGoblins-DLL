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
// A single marker in UNIFIED world space, pre-classified to a map group.
struct Marker
{
    float worldX = 0, worldZ = 0;
    // group = isDLC*2 | isUG : 0 base-overworld, 1 base-underground, 2 DLC-overworld,
    // 3 DLC-underground. The renderer draws only the OPEN group's markers.
    int group = 0;
    int srcArea = 0;            // original areaNo (diagnostics / converter choice)
    unsigned int color = 0xEB82E65Au; // packed ImU32 ABGR — default grace green
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
