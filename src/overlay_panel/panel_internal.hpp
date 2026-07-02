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

// ── Panel sections, in draw order ───────────────────────────────────────────
void draw_dev_icon_sections(const OverlayFrameCtx &ctx, Filter &f);  // P2b / migration / sprites / grace dbg
void draw_general_settings(const OverlayFrameCtx &ctx, Filter &f);   // master+save, flat toggles, scale, minimap
void draw_item_search(const OverlayFrameCtx &ctx, Filter &f);        // Find item / object
void draw_sections_categories(Filter &f);                            // Sections & categories + ERR integration
void draw_quest_browser(Filter &f);                                  // Quest navigation / Quest Browser
void draw_clustering(Filter &f);                                     // Clustering
void draw_dev_tools_danger(Filter &f);                               // Debug, Dev tools, Danger zone
} // namespace panel
} // namespace goblin::overlay
