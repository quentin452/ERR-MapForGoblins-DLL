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

// Draw the in-game minimap HUD (corner, north-up, player-centred) on the foreground
// draw list. Unlike render_markers this runs DURING GAMEPLAY (map closed) — it reads the
// live player position (goblin::get_player_map_pos) instead of the open-map view. Foundation:
// OVERWORLD only (area 60/61); no-op underground (player pos not yet reliable there) or when
// config::showMinimap is off / icons master off. screenW/H = backbuffer size (HUD anchor).
void draw_minimap(const std::vector<MarkerLayer *> &layers, void *atlas_texture, float screenW,
                  float screenH);
} // namespace goblin::worldmap
