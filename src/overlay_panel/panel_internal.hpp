#pragma once
// Internal surface of the F1 panel section files (big-files refactor item 1: the old
// ~1700-line draw_panel() split into per-section translation units). Everything here is
// render-module-internal — draw_panel() (goblin_overlay_render.cpp) owns the window frame
// + the settings-search box and calls the section functions below IN ORDER; each section
// file owns its own function-local static UI state, exactly as it did inside the monolith.
//
// NOT a public API: no GOBLIN_RENDER_API — these link only within the render module
// (or the single-DLL build, where the same sources compile together).

#include "goblin_overlay_render.hpp"  // OverlayFrameCtx + the D3D12-coupled helpers

#include <cstddef>
#include <string>
#include <vector>

namespace goblin::worldmap { class MarkerLayer; }

namespace goblin::overlay
{
// Shared overlay marker layers (graces + quest NPCs + one MapEntryLayer per category).
// Defined in goblin_overlay_render.cpp; the item search + its visited-page gate iterate it.
std::vector<goblin::worldmap::MarkerLayer *> &overlay_layers();

namespace panel
{
// ── Text-matching helpers (panel_util.cpp) ──────────────────────────────────
// Case-insensitive, accent-insensitive substring match (empty needle = match all).
bool contains_ci(const char *hay, const char *need);
// Word-order-independent match: every whitespace token of `query` must appear
// (case/accent-insensitive substring) somewhere in `hay`. Empty query = false.
bool matches_all_tokens(const std::string &hay, const char *query);

// ── Settings-search filter state ────────────────────────────────────────────
// One instance per draw_panel frame. Each block calls match() with its keyword
// string; while filtering, only matching blocks draw and hits counts them (so an
// all-miss query can say so — read on the NEXT frame, 1-frame lag is invisible).
struct Filter
{
    const char *q = "";
    bool filtering = false;
    int hits = 0;
    bool match(const char *keywords);
};

// ── Shared widgets (panel_util.cpp) ─────────────────────────────────────────
bool scale_control(const char *label, float *v, float lo, float hi,
                   float step, float step_fast, const char *fmt);
// On-screen keyboard popup button for gamepad text entry; writes into `buf`.
void draw_gamepad_keyboard_button(const char *popup_id, char *buf, size_t buf_size);
// Amber hint shown in the grace-debug sections when the candidate gate is off.
void grace_candidate_gate_warning();

// Draw category `c`'s resolved marker icon (native map-point → representative item icon → baked
// atlas cell → colored group dot), `size` px, at the cursor; advances the cursor by `size` so a
// following SameLine() places the label beside it. Mirrors map_renderer's IconSet::resolve tier
// order → the panel shows the SAME glyph the map draws (native/disk first, baked transitional,
// dot when nothing resolves). Native tiers only resolve once the sprite is resident (map opened);
// until then the baked cell or the dot stands in.
void draw_category_icon(const OverlayFrameCtx &ctx, int c, float size);
// Resolve category c's icon to {tex,uv} (native→atlas) for a draw-list AddImage (vmap canvas). false = none.
bool resolve_category_icon(const OverlayFrameCtx &ctx, int c, void *&tex, ImVec2 &uv0, ImVec2 &uv1);

// ── Panel sections, in draw order ───────────────────────────────────────────
void draw_dev_icon_sections(const OverlayFrameCtx &ctx, Filter &f);  // P2b / migration / sprites / grace dbg
void draw_general_settings(const OverlayFrameCtx &ctx, Filter &f);   // master+save, flat toggles, scale, minimap
void draw_item_search(const OverlayFrameCtx &ctx, Filter &f);        // Find item / object
void draw_sections_categories(const OverlayFrameCtx &ctx, Filter &f, bool with_err_integration = true); // + ERR block (skip on the vmap — native ERR pins don't show on an ImGui map)
bool markers_panel_open();  // vmap is hosting the categories sidebar → F1 skips its copy (single source)
void draw_quest_browser(Filter &f);                                  // Quest navigation / Quest Browser
void draw_clustering(Filter &f);                                     // Clustering
void draw_dev_tools_danger(Filter &f);                               // Debug, Dev tools, Danger zone
void draw_world_editor(Filter &f);                                   // World Editor (live loot/asset edit)

// MapForGoblins-owned VIRTUAL world map (mod page, not a native ER tab). draw_virtual_map draws the
// window (a pannable/zoomable world-space canvas) whenever virtual_map_open() is true; the toggle lives
// in the Dev tab. See docs/re/worldmap_new_page_spike_findings.md. Drawn as a sibling of the F1 panel.
void draw_virtual_map(const OverlayFrameCtx &ctx);
bool &virtual_map_open();
void virtual_map_request_fit();       // one-shot: frame the selected group's markers on next draw
void virtual_map_request_focus();     // one-shot: recenter on the player + switch to their page on next draw
void virtual_map_set_group(int g);    // 0..3 = base-OW / base-UG / DLC-OW / DLC-UG
// ER map ART tile (endgame phase-1a slice 2). needle = tile-name substring; wx1<=wx0 → auto grid quad.
void virtual_map_request_tile(const char *needle, float wx0, float wz0, float wx1, float wz1);
void virtual_map_clear_tiles();
// Load a whole dimension+LOD placed via the live converter affine (slice 3). Returns a status string.
std::string virtual_map_load_lod(int dim, int lod, int cap);
// Harvest LIVE resident tile rects (engine positions, no textures) → outline cells. Confirms alignment.
std::string virtual_map_load_resident();
// A3 recon: correlate live resident tile grid vs archive name-grid (archive-name↔runtime-cell). [TILERECON].
std::string virtual_map_tile_recon();
void virtual_map_service_pending_warp();  // POST-FRAME: execute a double-click-queued grace warp (safe pt)
int dump_markers_csv(const char *path);  // export all markers → CSV (offline procedural-style prototyping)
void virtual_map_set_view(float camX, float camZ, float zoom);  // dev/test: frame the canvas directly
void virtual_map_set_flip(bool flipX, bool flipZ);  // dev: world→screen axis signs (orientation calib)
int virtual_map_group();
} // namespace panel
} // namespace goblin::overlay
