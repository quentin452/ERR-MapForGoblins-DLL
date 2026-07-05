#pragma once
// map_renderer — the category-agnostic draw loop for the ImGui-rendered world map.
// Owns everything common to all marker types: read the live view, apply the baked
// frame delay (motion sync), gate by the open map group, world→map-space, project
// (goblin_projection.hpp), cull, draw. No-op when the world map isn't open.

#include "marker_layer.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

struct ImDrawList;  // fwd (imgui) — for draw_marker_glyph below

namespace goblin::worldmap
{
// Draw ONE marker with the full state-aware logic (icons per grace/collected/cleared state, dim, check,
// rune glow, altitude badge) at screen (px,py) on `dl`. Lets the virtual map reuse the native per-marker
// draw instead of duplicating it. atlas = baked-atlas SRV; native = config nativeItemIcons.
void draw_marker_glyph(ImDrawList *dl, const Marker &m, float px, float py, void *atlas, bool native, float half);

// Is this marker's item collected / boss cleared (the state that dims/hides it under collected_graying)?
// Exposed wrapper over the file-local marker_done — lets the vmap spiderfy drop collected/cleared members
// from a fan (they'd otherwise be blank slots: draw_marker_glyph hides them) → smaller, action-only fans.
bool marker_is_done(const Marker &m);

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
// nativeW/nativeH (optional, dx-bugs 2026-07-01): the harvested sprite's raw pixel rect dims
// before UV normalization, so the undiscovered-grace draw can auto-derive a size ratio.
void set_grace_sprite(void *tex, float u0, float v0, float u1, float v1, int nativeW = 0, int nativeH = 0);
// The ERR dungeon-style grace (for m.dungeon graces; null = fall back to the vanilla grace sprite).
void set_grace_dungeon_sprite(void *tex, float u0, float v0, float u1, float v1);

// True if the OS cursor was over an in-world control (e.g. a region chip) on the last
// render_markers pass. The overlay's WndProc reads this to consume the L-button PRESS for
// the game only when a chip is hovered (so map pan/select elsewhere is untouched), while
// still feeding the click to ImGui so the chip toggles even with the F1 panel closed.
bool inworld_hovered();

// ── Region on/off toggles — shared surface (native chips + virtual-map labels) ──
// The major-region show/hide flags (indexed by MAJOR_REGION_ANCHORS[i], seeded from
// config::regionToggles and persisted on Save) are owned by map_renderer. Expose a tiny
// read/set so the mod-owned Virtual World Map's region labels drive the SAME flags — a
// region toggled off on either surface stays in sync + persists. Both accessors seed the
// state first (the vmap may touch these before the native map has ever opened). An
// out-of-range index reads as "on" and writes are ignored.
bool region_enabled(int anchorIndex);
void region_set_enabled(int anchorIndex, bool on);

// ── Item search (F1 search bar) ──────────────────────────────────────────────
// Hand render_markers the set of marker name_ids whose resolved name matches the search query
// (null/empty = search inactive). Matching markers are ringed and pulled out of cluster piles.
// locateNameId (non-zero) latches a "point the cursor here" request for ONE marker; the next
// render captures that marker's backbuffer screen pos. Call each frame before render_markers.
void set_item_search(const std::unordered_set<int32_t> *matchNameIds, int32_t locateNameId);

// True while an item search is active (a non-empty match set was handed in). The overlay keeps
// drawing markers even with the icon master / a category toggled OFF so search hits stay revealed.
bool item_search_active();

// True while a clicked "locate" hasn't been satisfied yet — its marker is on a page that isn't open,
// so the request waits. The overlay shows a "switch to that page" banner; the pan fires automatically
// the frame that page opens. Cleared when satisfied or when the search is cleared.
bool locate_pending();

// After render_markers: if a locate request was satisfied this frame, returns true once and writes
// the matched marker's MARKER-SPACE coord (gU, gV) — the overlay pans the live map's view centre onto
// it (worldmap_probe::set_view_center). Returns false when no locate is pending / the marker wasn't
// on the open page this frame.
bool take_locate_pos(float *u, float *v);
// ── UI exclusion-zone editor (F1 "UI exclusion zones") ──────────────────────
// User-drawn rectangles where overlay icons are hidden on the world map, stored in
// cfg_uiExclusionRects_ref() as "x0,y0,x1,y1;..." in VIRTUAL-canvas units (1920x1080
// space) so they hold at every resolution. Edit mode: render_markers draws the zones
// and handles drag-create / right-click-delete directly on the open map.
void set_ui_rect_edit(bool on);
bool ui_rect_edit();
int ui_rect_count();
// Fills out[4] = x0,y0,x1,y1 (virtual units); false if index out of range.
bool ui_rect_get(int index, float out[4]);
void ui_rect_delete(int index);
void ui_rect_clear();
// ERR dial exclusion placement mode: draws the disc + time-pill live on the open map with
// drag handles that write the cfg::dial* globals (virtual units). ERR-only; "Save to INI" persists.
void set_dial_edit(bool on);
bool dial_edit();

} // namespace goblin::worldmap
