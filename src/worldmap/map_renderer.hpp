#pragma once
// map_renderer — the category-agnostic draw loop for the ImGui-rendered world map.
// Owns everything common to all marker types: read the live view, apply the baked
// frame delay (motion sync), gate by the open map group, world→map-space, project
// (goblin_projection.hpp), cull, draw. No-op when the world map isn't open.

#include "marker_layer.hpp"

#include <vector>

namespace goblin::worldmap
{
// Draw all visible layers' markers for the currently open map page. Call once per
// frame the overlay builds (from the Present hook). Must run inside an ImGui frame.
// atlas_texture = the category-icon atlas's ImGui texture id (GPU descriptor handle),
// or null to draw coloured-circle fallbacks. The overlay owns the texture; the
// renderer computes the per-icon UVs itself from goblin::overlay_icons.
// mouseX/mouseY = OS cursor in backbuffer px (-1 = no hover) → drives the marker/pile
// tooltip (name / location + count).
void render_markers(const std::vector<MarkerLayer *> &layers, void *atlas_texture = nullptr,
                    float mouseX = -1.f, float mouseY = -1.f);
} // namespace goblin::worldmap
