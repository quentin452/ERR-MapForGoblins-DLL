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
void render_markers(const std::vector<MarkerLayer *> &layers);
} // namespace goblin::worldmap
