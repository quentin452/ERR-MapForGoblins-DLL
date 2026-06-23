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

// Provide the harvested discovered-grace sprite so the overlay draws graces itself (RE e4b3f6a
// §6): tex = the grace texture's ImGui id, uv0/uv1 = the grace sub-rect within it. When set, grace
// markers draw with this sprite (discovered = full colour, undiscovered = grey) instead of being
// dropped (discovered) / circle-drawn (undiscovered). Pass tex=null to revert to the old behaviour.
// Call each frame before render_markers/draw_minimap.
void set_grace_sprite(void *tex, float u0, float v0, float u1, float v1);
// The ERR dungeon-style grace (for m.dungeon graces; null = fall back to the vanilla grace sprite).
void set_grace_dungeon_sprite(void *tex, float u0, float v0, float u1, float v1);

// True if the OS cursor was over an in-world control (e.g. a region chip) on the last
// render_markers pass. The overlay's WndProc reads this to consume the L-button PRESS for
// the game only when a chip is hovered (so map pan/select elsewhere is untouched), while
// still feeding the click to ImGui so the chip toggles even with the F1 panel closed.
bool inworld_hovered();
} // namespace goblin::worldmap
