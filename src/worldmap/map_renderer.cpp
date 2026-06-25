#include "map_renderer.hpp"
#include "category_meta.hpp"          // category_gpu_iconId (map-point symbol per category)

#include "goblin_bench.hpp"          // GOBLIN_BENCH_QUIET (per-frame render timing)
#include "goblin_projection.hpp"     // baked map-space → backbuffer projection
#include "goblin_worldmap_probe.hpp" // get_live_view()
#include "goblin_inject.hpp"         // ui::clustering_enabled / global_threshold / category_clustered
#include "goblin_overlay.hpp"        // overlay::native_item_icon (native GPU item-icon harvest)
#include "goblin_config.hpp"         // overlay marker scale config
#include "goblin_messages.hpp"       // lookup_text_utf8 (tooltip names)
#include "goblin_collected.hpp"      // is_original_row_collected (rune/ember graying)
#include "goblin_kindling.hpp"       // is_row_collected (kindling graying)
#include "goblin_major_regions.hpp"  // MAJOR_REGION_ANCHORS (region labels)
#include "goblin_name_regions.hpp"   // NAME_REGIONS (MapNameOverride debug viz)
#include "goblin_quest_gates.hpp"    // QUEST_GATES (quest-NPC gating)
#include "goblin_map_data.hpp"       // Category enum (WorldQuestNPC)

#include <string>
#include "generated_shared/goblin_overlay_icons.hpp" // ICON_CELLS / ATLAS dims

#include <imgui.h>
#include <spdlog/spdlog.h>  // [GROUPCHK] live-vs-baked group validation

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace goblin::worldmap
{
namespace
{
// Baked: the native GFx map layer is composited exactly 1 frame behind our Present
// sample, so map-bound markers need a 1-frame view delay to ride the map.
constexpr float kViewDelayFrames = 1.0f;

goblin::projection::ViewDelay<> g_view_delay;

// Probe LiveView → the pure projection View (keeps goblin_projection.hpp probe-free).
goblin::projection::View to_proj_view(const goblin::worldmap_probe::LiveView &v)
{
    goblin::projection::View pv;
    pv.panX = v.panX; pv.panZ = v.panZ; pv.zoom = v.zoom;
    pv.snapMidX = v.snapMidX; pv.snapMidZ = v.snapMidZ;
    return pv;
}


// Marker/glyph base sizes at 1920×1080; scaled by realH/1080 at draw time so they
// track the native GFx icons (which scale with the canvas) instead of looking
// oversized at low res / undersized at high res.
constexpr float kIconHalfBase = 10.f; // ~20px sprite at 1080 (native-ish)
constexpr float kGlyphRBase = 12.f;   // cluster pile disc radius at 1080

// Resolve a marker's atlas cell to UVs. Returns false if no atlas / key missing.
bool icon_uv(const char *key, ImVec2 &uv0, ImVec2 &uv1)
{
    using namespace goblin::overlay_icons;
    if (!key)
        return false;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
        {
            const IconCell &c = ICON_CELLS[i];
            uv0 = ImVec2((c.col * CELL) / (float)ATLAS_W, (c.row * CELL) / (float)ATLAS_H);
            uv1 = ImVec2(((c.col + 1) * CELL) / (float)ATLAS_W,
                         ((c.row + 1) * CELL) / (float)ATLAS_H);
            return true;
        }
    return false;
}

// ── Icon provider abstraction ────────────────────────────────────────────────
// Resolves a logical icon (a source-tagged key) to a drawable texture + UV rect, so
// draw sites stay backend-agnostic ("give me tex+UV for icon X") instead of hard-wiring
// the atlas. This is the seam for swapping/stacking pixel SOURCES: the baked atlas today,
// the game's native GPU item/map-point icons later (each a separate provider). resolve()
// returns false → the caller falls back (next provider, then a drawlist circle).
//
// The key is source-tagged (not a bare scalar) because the codebase has genuinely distinct
// pixel sources with different lookups: the atlas is keyed by a category string, the native
// backends by a numeric game IconId. Only the Atlas source exists for now.
struct IconKey
{
    enum Source { Atlas, ItemIcon, MapPoint } source = Atlas;
    const char *atlas_key = nullptr; // Atlas: category cell key ("show_bosses")
    int icon_id = -1;                // ItemIcon / MapPoint: numeric game IconId (future)
};

struct IconHandle
{
    ImTextureID tex = nullptr;
    ImVec2 uv0{}, uv1{};
    float scale = 1.0f; // draw-size multiplier (native map symbols are bigger than item dots)
};

struct IconProvider
{
    virtual ~IconProvider() = default;
    // Resolve key → handle. false = this provider can't supply it (caller falls back).
    virtual bool resolve(const IconKey &k, IconHandle &out) const = 0;
};

// Baked category atlas: holds the atlas texture, resolves Atlas-source keys via the
// generated ICON_CELLS table (the former icon_uv path). Guaranteed-coverage, synchronous.
struct AtlasProvider : IconProvider
{
    ImTextureID atlas = nullptr;
    explicit AtlasProvider(ImTextureID a) : atlas(a) {}
    bool resolve(const IconKey &k, IconHandle &out) const override
    {
        if (k.source != IconKey::Atlas || !atlas || !k.atlas_key)
            return false;
        if (!icon_uv(k.atlas_key, out.uv0, out.uv1))
            return false;
        out.tex = atlas;
        return true;
    }
};

// Native GPU item icon: the game's OWN inventory icon for an item/loot marker, harvested into
// an ImGui SRV by the overlay. Best-effort — resolves only for RESIDENT items and async (1-2
// frames), so it sits in FRONT of the atlas in the chain, never replacing it. Resolves an
// ItemIcon-source key by the marker's real iconId (Marker::icon_id).
struct ItemIconProvider : IconProvider
{
    bool resolve(const IconKey &k, IconHandle &out) const override
    {
        if (k.source != IconKey::ItemIcon || k.icon_id < 0)
            return false;
        void *tex = nullptr;
        float u0, v0, u1, v1;
        if (!goblin::overlay::native_item_icon(k.icon_id, tex, u0, v0, u1, v1))
            return false;
        out.tex = reinterpret_cast<ImTextureID>(tex);
        out.uv0 = ImVec2(u0, v0);
        out.uv1 = ImVec2(u1, v1);
        return true;
    }
};

// Native GPU map-point symbol: the game's OWN world-map symbol (MENU_MAP_<NN>) for a category
// that maps to one (category_gpu_iconId, sparse). Resolved via the FD4 image-repo rect copied
// into our SRV. Best-effort: resolves only after the world map opened + the symbol is resident.
struct MapPointProvider : IconProvider
{
    bool resolve(const IconKey &k, IconHandle &out) const override
    {
        if (k.source != IconKey::MapPoint || k.icon_id < 0)
            return false;
        void *tex = nullptr;
        float u0, v0, u1, v1;
        if (!goblin::overlay::native_map_point_icon(k.icon_id, tex, u0, v0, u1, v1))
            return false;
        out.tex = reinterpret_cast<ImTextureID>(tex);
        out.uv0 = ImVec2(u0, v0);
        out.uv1 = ImVec2(u1, v1);
        return true;
    }
};

// The active provider chain + per-marker resolution policy: native map-point symbol FIRST (for
// the categories that have a real game symbol via category_gpu_iconId), then the native item
// icon (the real inventory icon for loot, when resident), then the baked category atlas
// (guaranteed coverage), else the caller draws a circle. One instance per render pass.
struct IconSet
{
    AtlasProvider atlas;
    ItemIconProvider item;
    MapPointProvider mappoint;
    bool native; // config gate (config::nativeItemIcons): try the native backends first
    IconSet(ImTextureID a, bool native_on) : atlas(a), native(native_on) {}
    bool resolve(const Marker &m, IconHandle &out) const
    {
        if (native)
        {
            // World-feature categories with a mapped game symbol (sparse). Name-keyed first
            // (ERR custom MENU_MAP_ERR_*), then numeric MENU_MAP_<NN>.
            if (const char *mn = goblin::worldmap::category_gpu_icon_name(m.category))
            {
                void *t = nullptr; float a0, b0, a1, b1;
                if (goblin::overlay::native_map_point_icon_by_name(mn, t, a0, b0, a1, b1))
                {
                    out.tex = reinterpret_cast<ImTextureID>(t);
                    out.uv0 = ImVec2(a0, b0); out.uv1 = ImVec2(a1, b1);
                    out.scale = goblin::config::mapSymbolScale;
                    return true;
                }
            }
            int gid = goblin::worldmap::category_gpu_iconId(m.category);
            if (gid > 0 && mappoint.resolve(IconKey{IconKey::MapPoint, nullptr, gid}, out))
            {
                out.scale = goblin::config::mapSymbolScale;
                return true;
            }
            // Item/loot markers → the game's real inventory icon.
            if (m.icon_id >= 0 &&
                item.resolve(IconKey{IconKey::ItemIcon, nullptr, m.icon_id}, out))
                return true;
        }
        return atlas.resolve(IconKey{IconKey::Atlas, m.icon_key, -1}, out);
    }
};

// Desaturate toward luminance + halve alpha → the "collected" dim tint (packed ABGR).
unsigned int dim_color(unsigned int abgr)
{
    int a = (abgr >> 24) & 0xff, b = (abgr >> 16) & 0xff, g = (abgr >> 8) & 0xff, r = abgr & 0xff;
    int lum = (r * 54 + g * 183 + b * 19) >> 8; // ITU-R-ish luma
    r = (r + lum * 2) / 3; g = (g + lum * 2) / 3; b = (b + lum * 2) / 3;
    a /= 2;
    return ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}

// Is this marker's item collected / boss cleared? cleared_only reports the boss-clear
// sub-case (gets a checkmark). Event flags (cleared/loot) read the live game state;
// rune/ember/kindling use the collected-tracking sets (refreshed by the mod thread).
bool marker_done(const Marker &m, bool &cleared_only)
{
    cleared_only = m.cleared_flag && goblin::ui::read_event_flag((uint32_t)m.cleared_flag);
    if (cleared_only)
        return true;
    if (m.collected_flag && goblin::ui::read_event_flag((uint32_t)m.collected_flag))
        return true;
    if (m.row_id && (goblin::collected::is_original_row_collected(m.row_id) ||
                     goblin::kindling::is_row_collected(m.row_id)))
        return true;
    return false;
}

// A small green checkmark at the marker's top-right (cleared bosses, native-style).
void draw_check(ImDrawList *fg, ImVec2 p, float half)
{
    const float s = half * 0.8f;
    const ImVec2 c(p.x + half * 0.55f, p.y - half * 0.55f);
    fg->AddCircleFilled(c, s * 0.7f, IM_COL32(18, 40, 18, 200));
    const ImU32 col = IM_COL32(70, 225, 90, 255);
    fg->AddLine(ImVec2(c.x - s * 0.45f, c.y + s * 0.02f),
                ImVec2(c.x - s * 0.08f, c.y + s * 0.38f), col, 2.2f);
    fg->AddLine(ImVec2(c.x - s * 0.08f, c.y + s * 0.38f),
                ImVec2(c.x + s * 0.50f, c.y - s * 0.42f), col, 2.2f);
}

// Boss markers tinted red when redify_boss_icons is on (overlay port of the legacy
// red-skull iconId 374). The overlay has one WorldBosses category covering overworld +
// dungeon bosses; collected/cleared graying takes precedence (a dead boss grays). The
// legacy redify_dungeon (dungeon-ENTRANCE markers) has no overlay equivalent — the
// overlay projects dungeon CONTENTS, not a separate entrance marker — so it's native-only.
inline bool redify_boss(const Marker &m)
{
    return goblin::config::redifyBossIcons &&
           m.category == static_cast<int>(goblin::generated::Category::WorldBosses);
}

// Harvested discovered-grace sprite (set by the overlay via set_grace_sprite). When valid, grace
// markers draw with it instead of the circle/native hybrid. Render-thread only.
ImTextureID s_grace_tex = nullptr;
ImVec2 s_grace_uv0{}, s_grace_uv1{};
// ERR dungeon-style grace (MENU_MAP_ERR_GraceUnderground). Valid only when ERR is installed; used for
// DUNGEON graces (m.dungeon) in place of the vanilla bonfire. null → dungeon graces use s_grace_tex.
ImTextureID s_grace_dgn_tex = nullptr;
ImVec2 s_grace_dgn_uv0{}, s_grace_dgn_uv1{};
// Projection scale (zoom × canvas factor) for the grace GPU offset, refreshed each frame by
// render_markers AFTER the view delay finalizes zoom. graceOffsetX/Y is a native-vs-imgui
// calibration nudge expressed in 1920×1080-reference px; markers project as ·zoom·(real/virtual),
// so the offset must use the SAME factor or it drifts apart from the native pin as you zoom.
float s_grace_off_sx = 1.f, s_grace_off_sy = 1.f;
// Live view zoom (set each frame by render_markers after the view delay). The grace GPU
// icon scales WITH this so it tracks the map (zoom in → bigger, zoom out → smaller) instead
// of staying constant-px (which read as "huge when zoomed out, tiny when zoomed in").
float s_grace_zoom = 1.f;
// Zoom at which the grace size = graceIconScale·iconHalf (1×). Map zoom runs ~0.05..1; this
// reference centres the scaling so a mid zoom is unchanged. Calibration constant — tune if the
// size feels off across the zoom range.
constexpr float kGraceZoomRef = 0.25f;

// Draw one marker at backbuffer px p: the atlas icon if available, else a circle.
// half = icon half-size in px (resolution-scaled by the caller). When collected_graying
// is on, collected/cleared markers dim+desaturate (or hide if hide_collected), and
// cleared bosses get a green checkmark. Uncollected bosses redden when redify_boss_icons.
void draw_marker(ImDrawList *fg, const Marker &m, ImVec2 p, const IconSet &icons, float half)
{
    // Grace marker (discover_flag set only on graces): with grace_overlay the overlay draws it
    // itself — discovered (rested) = full colour, undiscovered = grey. Source per grace_gpu_sprite:
    // the live engine sprite (s_grace_tex, time-tinted) or the mod's baked atlas icon (clean). Needs
    // native-pin suppression to avoid doubling. Graces aren't collectible → no graying path.
    if (m.discover_flag && goblin::config::graceOverlay)
    {
        // Discovered = a green check (same "done" language as cleared bosses / collected loot), NOT a
        // faded tint — ImGui can't desaturate a texture, and a check reads far clearer than low opacity.
        bool disc = goblin::ui::read_event_flag(static_cast<uint32_t>(m.discover_flag));
        ImU32 t = IM_COL32(255, 255, 255, 255);   // grace icon always full colour
        if (goblin::config::graceGpuSprite && s_grace_tex)
        {
            // SIMPLIFIED: the BASE grace sprite is drawn for EVERY grace (cave/dungeon/overworld
            // Per-type grace icon (UNPARKED): overworld graces draw the bonfire (s_grace_tex), the
            // ERR underground/cave graces (m.dungeon = BonfireWarpParam.iconId==44) draw the ERR
            // dungeon sprite (s_grace_dgn_tex = MENU_MAP_ERR_GraceUnderground). Both now come from the
            // same harvest path so the old GPU/CPU-unify blocker is gone. Falls back to the bonfire if
            // the dungeon sprite isn't harvested yet.
            ImTextureID gt = s_grace_tex;
            ImVec2 u0 = s_grace_uv0;
            ImVec2 u1 = s_grace_uv1;
            if (m.dungeon && s_grace_dgn_tex)
            {
                gt = s_grace_dgn_tex;
                u0 = s_grace_dgn_uv0;
                u1 = s_grace_dgn_uv1;
            }
            // Scale WITH the map zoom (clamped so it never vanishes / overflows). zf = 1 at
            // kGraceZoomRef; zoom in → >1 (bigger), zoom out → <1 (smaller). graceIconScale is
            // the user's overall-size dial on top.
            float zf = s_grace_zoom / kGraceZoomRef;
            if (zf < 0.3f) zf = 0.3f;
            if (zf > 2.0f) zf = 2.0f;          // cap high-zoom growth (graces were too big zoomed in)
            float gh = half * goblin::config::graceIconScale * zf;
            // Optional offset → shift the imgui grace beside the game's NATIVE pin for side-by-side
            // calibration (0 = on top). Scaled by the live projection factor (zoom × canvas) so the
            // nudge tracks the native pin across zoom levels instead of drifting (it's stored in
            // 1920×1080-reference px, same convention as the projection bias).
            float gx = p.x + goblin::config::graceOffsetX * s_grace_off_sx,
                  gy = p.y + goblin::config::graceOffsetY * s_grace_off_sy;
            fg->AddImage(gt, ImVec2(gx - gh, gy - gh), ImVec2(gx + gh, gy + gh), u0, u1, t);
            if (disc)
                draw_check(fg, ImVec2(gx, gy), gh);   // discovered → green check (same as cleared bosses)
            return;
        }
        IconHandle ih;
        if (icons.resolve(m, ih))
            fg->AddImage(ih.tex, ImVec2(p.x - half, p.y - half), ImVec2(p.x + half, p.y + half),
                         ih.uv0, ih.uv1, t);
        else
        {
            float cr = half * 0.45f;
            fg->AddCircleFilled(p, cr, disc ? m.color : dim_color(m.color));
            fg->AddCircle(p, cr, IM_COL32(0, 0, 0, 220), 0, 1.5f);
        }
        return;
    }
    bool cleared = false, done = false;
    if (goblin::config::collectedGraying)
    {
        done = marker_done(m, cleared);
        if (done && goblin::config::hideCollected)
            return; // legacy-style: hide collected/cleared entirely
    }
    // Spoiler-free (anonymous_loot): lot-backed loot draws as a neutral gray "?" disc,
    // hiding the item's icon/colour. Collected ones still gray; category gate unchanged.
    if (goblin::config::anonymousLoot && m.lot_backed)
    {
        float cr = half * 0.5f;
        const ImU32 fill = done ? IM_COL32(120, 120, 120, 120) : IM_COL32(155, 155, 160, 215);
        fg->AddCircleFilled(p, cr, fill);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, done ? 120 : 220), 0, 1.5f);
        const char *q = "?";
        ImVec2 ts = ImGui::CalcTextSize(q);
        fg->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f),
                    IM_COL32(25, 25, 30, done ? 150 : 255), q);
        return;
    }
    const bool red = !done && redify_boss(m);
    const ImU32 tint = done ? IM_COL32(150, 150, 150, 130)
                            : (red ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 255, 255, 255));

    IconHandle ih;
    if (icons.resolve(m, ih))
    {
        const float hh = half * ih.scale;
        fg->AddImage(ih.tex, ImVec2(p.x - hh, p.y - hh), ImVec2(p.x + hh, p.y + hh),
                     ih.uv0, ih.uv1, tint);
    }
    else
    {
        float cr = half * 0.45f;
        const ImU32 fill = done ? dim_color(m.color) : (red ? IM_COL32(235, 70, 70, 255) : m.color);
        fg->AddCircleFilled(p, cr, fill);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, done ? 120 : 220), 0, 1.5f);
    }
    if (cleared)
        draw_check(fg, p, half);
}

// Unified world coords → map-space (the frame project_screen expects).
void world_to_mapspace_xy(float worldX, float worldZ, float &gU, float &gV)
{
    // EXACT world→map-space (agent RE 0a30738): origin (7168,16384), bias 128,
    // scale 1.0, Z-flipped: mapX = worldX − 7040 ; mapZ = −worldZ + 16512.
    gU = worldX - 7040.0f;
    gV = -worldZ + 16512.0f;
}
inline void world_to_mapspace(const Marker &m, float &gU, float &gV)
{
    world_to_mapspace_xy(m.worldX, m.worldZ, gU, gV);
}

// Project a marker to map-space. With config::liveProjection, use the engine's OWN
// projection (worldmap_probe::project on the live WorldMapViewModel) — folds LegacyConv
// for dungeons/underground exactly like the native map. Result is cached per marker
// (the converter affine is static, so it's valid across map reopens); retries until the
// map is open + the VM resolves. Falls back to the baked affine until then / if the engine
// doesn't place the area (e.g. m19 Chapel — no converter accepts it).
inline void project_marker(const Marker &m, float &gU, float &gV)
{
    if (goblin::config::liveProjection && m.raw_area >= 0)
    {
        if (m.live_state != 1)
        {
            float u, v;
            int pg = -1;
            if (goblin::worldmap_probe::project(m.raw_area, m.raw_gx, m.raw_gz, m.raw_px,
                                                m.raw_pz, u, v, pg))
            {
                m.live_u = u;
                m.live_v = v;
                m.live_page = pg;
                m.live_state = 1;
                // Validate the LIVE-derived group vs the baked m.group. group =
                // (page==10?DLC:0) | (area==12?UG:0) — DLC areas 40-43 are plain DLC
                // (no separate underground page), matching marker_group_from.
                int live_grp = (pg == 10 ? 2 : 0) | (m.raw_area == 12 ? 1 : 0);
                if (live_grp != m.group)
                {
                    static int n = 0;
                    if (n++ < 30)
                        spdlog::warn("[GROUPCHK] mismatch area{} grid({},{}) live_page={} "
                                     "live_grp={} baked_grp={}",
                                     m.raw_area, m.raw_gx, m.raw_gz, pg, live_grp, m.group);
                }
            }
        }
        if (m.live_state == 1)
        {
            gU = m.live_u;
            gV = m.live_v;
            return;
        }
    }
    world_to_mapspace(m, gU, gV);
}

// Project a NON-marker point (grace-anchor pile, region label) to map-space. With
// config::liveProjection, call the engine from the point's RAW per-area frame
// (rawX/rawZ = gridX*256+pos in the point's OWN area — NOT pre-folded) by decomposing
// back to grid+pos; else the baked affine on the already-folded world coords. No cache
// (these are few per frame). area < 0 → baked.
inline void project_raw(int area, float rawX, float rawZ, float bakedWX, float bakedWZ,
                        float &gU, float &gV)
{
    if (goblin::config::liveProjection && area >= 0)
    {
        int gx = (int)std::floor(rawX / 256.0f), gz = (int)std::floor(rawZ / 256.0f);
        int pg = -1;
        if (goblin::worldmap_probe::project(area, gx, gz, rawX - gx * 256.0f,
                                            rawZ - gz * 256.0f, gU, gV, pg))
            return;
    }
    world_to_mapspace_xy(bakedWX, bakedWZ, gU, gV);
}

// A projected marker awaiting the clustering decision.
struct ScreenMarker
{
    ImVec2 p;
    const Marker *m;
};

// Best hovered item this frame (for the tooltip). bestd = squared px distance.
struct Hover
{
    float bestd = 1e30f;
    ImVec2 pos;
    std::string text;
};
// If `mouse` is within radius r of p and closer than the current best, take it.
// PERF: the label is built LAZILY (make_label is only invoked when this point is a new
// closest-within-radius candidate), so we don't construct an FMG/UTF8 string for every
// marker every frame — only the handful near the cursor. make_label() returns the label
// std::string; an empty result is skipped (same as the old text.empty() guard), so a
// label-less marker never blocks a farther named one. Behaviour is identical to building
// the string eagerly; only the work moved behind the cheap distance test.
template <class MakeLabel>
void hover_test(Hover &h, ImVec2 mouse, ImVec2 p, float r, MakeLabel make_label)
{
    if (mouse.x < 0) return;
    float dx = mouse.x - p.x, dy = mouse.y - p.y, d = dx * dx + dy * dy;
    if (d > r * r || d >= h.bestd) return;   // not within radius, or not a new best → no label build
    std::string t = make_label();
    if (t.empty()) return;
    h.bestd = d; h.pos = p; h.text = std::move(t);
}
// One marker's tooltip = its item/marker name + its location (nearest-grace region),
// like the native map. Either may be empty; empty if both are.
std::string marker_label(const Marker &m)
{
    std::string loc = goblin::lookup_text_utf8(m.loc_pname);
    // Spoiler-free: don't leak the item name — just "?" (+ its location, like native).
    if (goblin::config::anonymousLoot && m.lot_backed)
        return loc.empty() ? std::string("?") : ("?\n" + loc);
    std::string name = goblin::lookup_text_utf8(m.name_id);
    if (name.empty()) return loc;
    if (loc.empty()) return name;
    return name + "\n" + loc; // item name, then its location on the next line
}
// A cluster pile's tooltip = its location name (if any) + the member count. Shows
// "<remaining>/<total> left" while some are uncollected, "<total> markers" otherwise.
std::string pile_label(int loc_pname, int remaining, int total)
{
    std::string loc = goblin::lookup_text_utf8(loc_pname);
    std::string n = (remaining == total)
                        ? (std::to_string(total) + " markers")
                        : (std::to_string(remaining) + "/" + std::to_string(total) + " left");
    return loc.empty() ? n : (loc + "\n" + n);
}
// Draw a small dark tooltip box near the cursor on the foreground draw list.
void draw_tooltip(ImDrawList *fg, ImVec2 at, const std::string &text)
{
    if (text.empty()) return;
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 p0(at.x + 16, at.y + 6), p1(p0.x + ts.x + 12, p0.y + ts.y + 8);
    fg->AddRectFilled(p0, p1, IM_COL32(20, 20, 26, 235), 4.f);
    fg->AddRect(p0, p1, IM_COL32(255, 255, 255, 70), 4.f);
    fg->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(245, 245, 245, 255), text.c_str());
}

// Draw a cluster pile glyph (filled disc + remaining-member count) at screen point c.
// depleted = every member collected → green-tinted disc (mirrors the legacy native
// CLUSTER_DONE icon) so a finished area reads as done at a glance.
void draw_cluster_glyph(ImDrawList *fg, ImVec2 c, int n, float r, bool depleted)
{
    const ImU32 fill = depleted ? IM_COL32(28, 44, 30, 205) : IM_COL32(40, 42, 52, 235);
    const ImU32 ring = depleted ? IM_COL32(70, 205, 95, 225) : IM_COL32(255, 255, 255, 230);
    const ImU32 txt = depleted ? IM_COL32(150, 225, 155, 255) : IM_COL32(255, 255, 255, 255);
    fg->AddCircleFilled(c, r, fill);
    fg->AddCircle(c, r, ring, 0, 2.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    fg->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), txt, buf);
}

// Group clustered markers by their nearest-grace key (matches the native map's
// by-location clustering, NOT screen proximity — markers sharing a grace pile even
// when spread out). A group with MORE than `threshold` members draws ONE glyph at the
// member screen-centroid; smaller groups draw their members normally. Off-screen
// piles/markers are culled. realW/realH = backbuffer size for the cull.
void draw_clusters(ImDrawList *fg, const std::vector<ScreenMarker> &items, int threshold,
                   const IconSet &icons, float realW, float realH,
                   const goblin::projection::View &view, bool dist_eligible,
                   float iconHalf, float glyphR, ImVec2 mouse, Hover &hover)
{
    namespace proj = goblin::projection;
    auto on_screen = [&](const ImVec2 &p) {
        return !(p.x < -32 || p.y < -32 || p.x > realW + 32 || p.y > realH + 32);
    };
    std::unordered_map<int, std::vector<int>> groups; // cluster_key → member indices
    groups.reserve(items.size());
    for (int i = 0; i < (int)items.size(); ++i)
        groups[items[i].m->cluster_key].push_back(i);

    // Distance-adaptive: when on, the per-location threshold ramps from near_thr (full
    // detail around the player) to base_thr (clustered far away) over near→far radius,
    // SAME overworld page only (world coords aren't comparable across pages, and the
    // underground player float is unreliable). This is its own feature: enabling it
    // OVERRIDES per-category opt-in (every category clusters by distance — handled at
    // the eligibility check in render_markers).
    namespace cfg = goblin::config;
    const bool dist_adaptive = cfg::clusterDistanceAdaptive && dist_eligible;
    const int base_thr = threshold;
    int near_thr = (int)cfg::clusterNearThreshold;
    if (near_thr < 1) near_thr = 1;
    const float near_u = cfg::clusterNearRadius * 256.0f;
    const float far_u = cfg::clusterFarRadius * 256.0f;
    int player_area = -1;
    float pwx = 0, pwz = 0;
    const bool have_player =
        dist_adaptive && goblin::get_player_map_pos(player_area, pwx, pwz); // projected (debug rings)
    // The distance ramp measures player↔grace in the RAW per-area frame (gridX*256+pos, NO
    // projection) so it's correct on EVERY page — the projected unified frame OVERLAPS
    // overworld↔underground, which gave garbage distances in Siofra. Gate on same raw area.
    int raw_pa = -1;
    float raw_pwx = 0, raw_pwz = 0;
    const bool have_raw = dist_adaptive && goblin::get_player_raw_pos(raw_pa, raw_pwx, raw_pwz);

    // DEBUG viz (config cluster_debug_radius): player marker + near/far rings so you can
    // SEE where the distance ramp engages. Draws on every page the ramp runs (incl. the
    // projected underground); per-pile d=/thr= below shows the ramp's decision.
    const bool dbg_radius = goblin::config::clusterDebugRadius && have_player;
    if (dbg_radius)
    {
        float gU, gV;
        world_to_mapspace_xy(pwx, pwz, gU, gV);
        proj::Px pp = proj::project_screen(gU, gV, view, realW, realH);
        const ImVec2 ppx(pp.x, pp.y);
        auto ring_px = [&](float r) {
            float u, v;
            world_to_mapspace_xy(pwx + r, pwz, u, v);
            proj::Px e = proj::project_screen(u, v, view, realW, realH);
            return std::fabs(e.x - pp.x);
        };
        fg->AddCircle(ppx, ring_px(near_u), IM_COL32(60, 230, 90, 210), 64, 2.f);  // near=detail
        fg->AddCircle(ppx, ring_px(far_u), IM_COL32(255, 90, 30, 210), 64, 2.f);   // far=clustered
        fg->AddCircleFilled(ppx, 6.f, IM_COL32(255, 0, 255, 255));
        fg->AddCircle(ppx, 6.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        char b[48];
        std::snprintf(b, sizeof(b), "player A%d", player_area);
        fg->AddText(ImVec2(ppx.x + 8, ppx.y - 6), IM_COL32(255, 120, 255, 255), b);
    }

    // Pass 1: resolve each pile's GRACE anchor + its (distance-adaptive) threshold.
    // Sub-threshold groups draw their members normally; denser ones become a pile.
    struct Pile { ImVec2 g; int count; int total; int loc_pname; int thr; };
    std::vector<Pile> piles;
    for (auto &kv : groups)
    {
        const auto &idxs = kv.second;
        // Grace anchor (projected world + page) for the pile PLACEMENT below.
        int garea = -1;
        float gwx = 0, gwz = 0;
        const bool has_anchor = goblin::grace_anchor_world(kv.first, garea, gwx, gwz);
        // Per-location threshold. Distance-adaptive: Euclidean ramp near_thr→base_thr over
        // the near→far radius, measured in the RAW per-area frame (player + grace must share
        // the same raw area; underground sub-maps stay distinct via gridX*256). Otherwise
        // flat base_thr (normal threshold clustering).
        int thr = base_thr;
        float dbg_d = -1.f; // raw euclid distance player→grace (for the debug viz)
        int graw_a = -1; float graw_x = 0, graw_z = 0;
        if (have_raw && goblin::grace_anchor_raw(kv.first, graw_a, graw_x, graw_z) &&
            graw_a == raw_pa && far_u > near_u)
        {
            const float dx = graw_x - raw_pwx, dz = graw_z - raw_pwz;
            const float d = std::sqrt(dx * dx + dz * dz);
            dbg_d = d;
            float t = (d - near_u) / (far_u - near_u);
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            thr = (int)std::lround(near_thr + t * (base_thr - near_thr));
            if (thr < 1) thr = 1;
        }
        if ((int)idxs.size() <= thr)
        {
            for (int i : idxs)
                if (on_screen(items[i].p))
                {
                    draw_marker(fg, *items[i].m, items[i].p, icons, iconHalf);
                    const Marker *mm = items[i].m;
                    hover_test(hover, mouse, items[i].p, iconHalf,
                               [&] { return marker_label(*mm); });
                }
            continue;
        }
        // Pile AT its grace (correctly placed), not the member centroid (which drifts
        // into the sea). Fall back to the centroid only if the anchor is bad.
        ImVec2 c;
        if (has_anchor)
        {
            // Place the pile at its grace via the live engine projection (from the grace's
            // RAW per-area frame, so legacy/UG graces fold correctly); baked fallback.
            float gU, gV;
            int ra = -1; float rx = 0, rz = 0;
            bool raw_ok = goblin::grace_anchor_raw(kv.first, ra, rx, rz);
            project_raw(raw_ok ? ra : -1, rx, rz, gwx, gwz, gU, gV);
            proj::Px gp = proj::project_screen(gU, gV, view, realW, realH);
            c = ImVec2(gp.x, gp.y);
        }
        else
        {
            float sx = 0, sy = 0;
            for (int i : idxs) { sx += items[i].p.x; sy += items[i].p.y; }
            c = ImVec2(sx / idxs.size(), sy / idxs.size());
        }
        // DEBUG (cluster_debug_radius): draw the pile ANCHOR + lines to every member +
        // the location name → SEE where a pile is placed vs its markers. Green = placed at
        // its grace anchor; red = anchor missing → member centroid (the drift/mis-place
        // cases, e.g. the Chapel cluster + underground miscalc). Lines that fan way out =
        // the pile sits far from its members.
        if (goblin::config::debugClusterAnchors)
        {
            const ImU32 dcol = has_anchor ? IM_COL32(40, 230, 170, 170)
                                          : IM_COL32(255, 70, 70, 220);
            for (int i : idxs)
                fg->AddLine(c, items[i].p, dcol, 1.0f);
            fg->AddCircleFilled(c, 4.f, dcol);
            fg->AddCircle(c, 4.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
            std::string nm = goblin::lookup_text_utf8(items[idxs[0]].m->loc_pname);
            char db[140];
            if (dbg_d >= 0.f) // distance-adaptive engaged: show dist + chosen threshold
                std::snprintf(db, sizeof(db), "%s%s [%d] d=%.0f thr=%d",
                              has_anchor ? "" : "CENTROID ", nm.c_str(), (int)idxs.size(),
                              dbg_d, thr);
            else
                std::snprintf(db, sizeof(db), "%s%s [%d] thr=%d",
                              has_anchor ? "" : "CENTROID ", nm.c_str(), (int)idxs.size(), thr);
            fg->AddText(ImVec2(c.x + 7, c.y + 5), dcol, db);
        }
        // Pile count = UNCOLLECTED members (depletion), so the glyph reflects progress
        // instead of the static total. When collected_graying is off, show the full
        // total. marker_done = the same collected/cleared predicate used for graying.
        int total = (int)idxs.size();
        int remaining = total;
        if (goblin::config::collectedGraying)
        {
            remaining = 0;
            for (int i : idxs)
            {
                bool co;
                if (!marker_done(*items[i].m, co))
                    ++remaining;
            }
        }
        piles.push_back({c, remaining, total, items[idxs[0]].m->loc_pname, thr});
    }

    // Pass 2: nudge each pile off its grace icon (so both stay visible), in the cardinal
    // direction pointing AWAY from its nearest neighbouring grace — so a pile next to
    // other graces doesn't land on one. The direction is derived from the screen vector
    // between two graces, whose ANGLE is zoom-invariant (zoom scales both graces'
    // positions by the same factor), so the chosen direction is STABLE across zoom —
    // no flipping when you zoom in/out. Isolated piles default to "below".
    // Offset depends ONLY on the cluster glyph size (not iconHalf) so the category
    // icon scale never moves the piles — clusters are governed by the cluster scale.
    const float OFF = glyphR * 2.0f;
    for (size_t i = 0; i < piles.size(); ++i)
    {
        int nn = -1;
        float nnd = 1e30f;
        for (size_t j = 0; j < piles.size(); ++j)
        {
            if (j == i) continue;
            float dx = piles[i].g.x - piles[j].g.x, dy = piles[i].g.y - piles[j].g.y;
            float d = dx * dx + dy * dy;
            if (d < nnd) { nnd = d; nn = (int)j; }
        }
        ImVec2 off(0.f, OFF); // default: below
        if (nn >= 0)
        {
            float vx = piles[i].g.x - piles[nn].g.x, vy = piles[i].g.y - piles[nn].g.y;
            if (std::fabs(vx) > std::fabs(vy))
                off = ImVec2(vx > 0 ? OFF : -OFF, 0.f);
            else
                off = ImVec2(0.f, vy > 0 ? OFF : -OFF);
        }
        ImVec2 best(piles[i].g.x + off.x, piles[i].g.y + off.y);
        if (on_screen(best))
        {
            const bool depleted = (piles[i].count == 0 && piles[i].total > 0);
            draw_cluster_glyph(fg, best, piles[i].count, glyphR, depleted);
            if (dbg_radius)
            {
                // The per-location threshold this pile used (near_thr vs base_thr =
                // distance/tab decision) — drawn above the glyph in cyan.
                char tb[24];
                std::snprintf(tb, sizeof(tb), "thr%d", piles[i].thr);
                fg->AddText(ImVec2(best.x - glyphR, best.y - glyphR - 14.f),
                            IM_COL32(80, 230, 255, 255), tb);
            }
            const Pile &pl = piles[i];
            hover_test(hover, mouse, best, glyphR,
                       [&] { return pile_label(pl.loc_pname, pl.count, pl.total); });
            // Location name centred under the glyph (shadowed for readability on the busy map).
            std::string loc = goblin::lookup_text_utf8(piles[i].loc_pname);
            if (!loc.empty())
            {
                ImVec2 ts = ImGui::CalcTextSize(loc.c_str());
                ImVec2 tp(best.x - ts.x * 0.5f, best.y + glyphR + 2.f);
                fg->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 205), loc.c_str());
                fg->AddText(tp, IM_COL32(255, 255, 255, 235), loc.c_str());
            }
        }
    }
}

// Does a major-region anchor (its `area` page id) belong to the currently OPEN map
// group? Anchor pages: 10/60 = base overworld, 12 = base underground, 61 = DLC
// overworld. (No major-region anchor exists for the DLC underground, group 3.)
bool region_in_group(uint8_t area, int open_grp)
{
    switch (open_grp)
    {
    case 0: return area == 10 || area == 60; // base overworld
    case 1: return area == 12;               // base underground
    case 2: return area == 61;               // DLC overworld
    default: return false;                   // group 3 (DLC UG): none
    }
}

// ── In-world region on/off toggles (v1) ──────────────────────────────────────
// The major-region NAME doubles as a clickable in-world chip: click it to hide that
// region's markers. This is the first "diegetic" control — projected onto the map
// (rides pan/zoom) instead of a screen-fixed panel. v1 scope: runtime-only state (not
// persisted), interactive only while the F1 panel feeds ImGui mouse (g_show); F1-closed
// clicking is the next step (needs a WndProc mouse feed when the world map is open).
constexpr int kMaxRegions = 32; // MAJOR_REGION_ANCHOR_COUNT is a runtime const; cap the arrays.
bool g_region_on[kMaxRegions];
bool g_region_init = false;
bool s_inworld_hot = false; // cursor over an in-world control this frame (future input gating)

// Per-frame cache of each major-region anchor's projected map-space (gU,gV) + its map
// group — filled once by compute_region_proj, reused by the chip drawer AND the marker
// gate (so the gate doesn't re-project per marker).
float s_region_u[kMaxRegions], s_region_v[kMaxRegions];
int s_region_grp[kMaxRegions];
bool s_region_valid[kMaxRegions];

// Seed g_region_on from the persisted INI blob (config::regionToggles): default all-on,
// then apply each '0'/'1' char in anchor order. Runs once (first frame, after load_config).
void ensure_region_init()
{
    if (g_region_init)
        return;
    for (int i = 0; i < kMaxRegions; ++i)
        g_region_on[i] = true;
    const std::string &s = goblin::config::regionToggles;
    const int n = (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT;
    for (int i = 0; i < n && i < kMaxRegions && i < (int)s.size(); ++i)
        g_region_on[i] = (s[i] != '0');
    g_region_init = true;
}

// Serialize g_region_on back into config::regionToggles (one '0'/'1' per anchor) so the
// next in-game Save persists it. Called on each toggle (cheap; the file write is Save-gated).
void persist_regions()
{
    const int n = (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT < kMaxRegions
                      ? (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT
                      : kMaxRegions;
    std::string s;
    s.reserve(n);
    for (int i = 0; i < n; ++i)
        s.push_back(g_region_on[i] ? '1' : '0');
    goblin::config::regionToggles = s;
}

// Project every major-region anchor to map-space for the current frame (same transform as
// draw_region_labels: legacy-conv world pos → live/baked map-space). group = the anchor's
// map group, so the gate can match markers to same-group regions only.
void compute_region_proj()
{
    using namespace goblin::generated;
    ensure_region_init(); // before any_region_off()/the gate read g_region_on (frame 1)
    const int n = (int)MAJOR_REGION_ANCHOR_COUNT < kMaxRegions ? (int)MAJOR_REGION_ANCHOR_COUNT
                                                               : kMaxRegions;
    for (int i = 0; i < n; ++i)
    {
        const MajorRegionAnchor &a = MAJOR_REGION_ANCHORS[i];
        int ga;
        float wx, wz;
        goblin::marker_world_pos(a.area, a.gx, a.gz, a.px, a.pz, ga, wx, wz,
                                 /*conv_underground=*/true);
        float gU, gV;
        bool placed = false;
        if (goblin::config::liveProjection)
        {
            int pg = -1;
            placed = goblin::worldmap_probe::project(a.area, a.gx, a.gz, a.px, a.pz, gU, gV, pg);
        }
        if (!placed)
            world_to_mapspace_xy(wx, wz, gU, gV);
        s_region_u[i] = gU;
        s_region_v[i] = gV;
        s_region_grp[i] = goblin::marker_group_from(a.area, ga);
        s_region_valid[i] = true;
    }
}

// Is any region currently toggled OFF? (Skips the per-marker nearest-region gate entirely
// when everything is on — the common case, zero added cost.)
bool any_region_off()
{
    const int n = (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT;
    for (int i = 0; i < n && i < kMaxRegions; ++i)
        if (!g_region_on[i])
            return true;
    return false;
}

// Nearest same-group region anchor to a marker's map-space point (Voronoi region
// assignment). Returns its index, or -1 if no same-group anchor exists.
int nearest_region(int marker_grp, float mu, float mv)
{
    const int n = (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT;
    int best = -1;
    float bd = FLT_MAX;
    for (int i = 0; i < n && i < kMaxRegions; ++i)
    {
        if (!s_region_valid[i] || s_region_grp[i] != marker_grp)
            continue;
        const float du = s_region_u[i] - mu, dv = s_region_v[i] - mv;
        const float d = du * du + dv * dv;
        if (d < bd)
        {
            bd = d;
            best = i;
        }
    }
    return best;
}

// Draw the coarse major-region names (Limgrave, Caelid, ...) as in-world on/off CHIPS on
// the open page. Each anchor's world centre projects exactly like a marker; the name is
// drawn large + shadowed, centred on the anchor, on a pill background. Hovering highlights;
// clicking (left, F1 open) toggles g_region_on[i] — OFF dims the name + strikes it through,
// and the marker loop hides that region's markers. Off-screen chips are culled. mouse =
// OS cursor in backbuffer px (-1 = none).
void draw_region_labels(ImDrawList *fg, int open_grp,
                        const goblin::projection::View &view, float realW, float realH,
                        float uiScale, ImVec2 mouse)
{
    namespace proj = goblin::projection;
    using namespace goblin::generated;
    ensure_region_init();

    // Zoom-LOD fade: chips are an OVERVIEW control — full at low zoom (whole map), gone once
    // zoomed in (markers dominate, chips would clutter/overlap). Fade alpha over a band instead
    // of a hard cutoff (no flicker near the threshold). Map zoom runs ~0.05..1.0.
    constexpr float kChipZoomFull = 0.20f; // ≤ : full opacity
    constexpr float kChipZoomHide = 0.40f; // ≥ : invisible
    float fade = (kChipZoomHide - view.zoom) / (kChipZoomHide - kChipZoomFull);
    fade = fade < 0.f ? 0.f : (fade > 1.f ? 1.f : fade);
    if (fade <= 0.f)
        return; // fully zoomed in → no chips drawn, none clickable

    auto with_alpha = [fade](ImU32 c) {
        unsigned a = (unsigned)(((c >> IM_COL32_A_SHIFT) & 0xFF) * fade + 0.5f);
        return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    const float fontSize = ImGui::GetFontSize() * 1.6f * uiScale;
    const ImU32 shadow = with_alpha(IM_COL32(0, 0, 0, 190));
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ImFont *font = ImGui::GetFont();
    const int n = (int)MAJOR_REGION_ANCHOR_COUNT < kMaxRegions ? (int)MAJOR_REGION_ANCHOR_COUNT
                                                               : kMaxRegions;
    for (int i = 0; i < n; ++i)
    {
        // Only chips on the open page (same group as the cached projection). Map-space comes
        // from the per-frame cache (compute_region_proj), so the chip and the marker gate
        // share one projection.
        if (!s_region_valid[i] || s_region_grp[i] != open_grp)
            continue;
        const MajorRegionAnchor &a = MAJOR_REGION_ANCHORS[i];
        proj::Px p = proj::project_screen(s_region_u[i], s_region_v[i], view, realW, realH);
        if (p.x < -64 || p.y < -32 || p.x > realW + 64 || p.y > realH + 32)
            continue;

        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, a.name);
        ImVec2 tp(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f);
        const float pad = 6.f;
        ImVec2 r0(tp.x - pad, tp.y - pad), r1(tp.x + ts.x + pad, tp.y + ts.y + pad);
        const bool hot = mouse.x >= r0.x && mouse.x <= r1.x && mouse.y >= r0.y && mouse.y <= r1.y;
        if (hot)
            s_inworld_hot = true;
        if (hot && clicked)
        {
            g_region_on[i] = !g_region_on[i];
            persist_regions(); // mirror into config::regionToggles for the next Save
        }
        const bool on = g_region_on[i];

        // Pill background (warmer when hovered; reddish when off) + hover outline. All
        // alphas ride the zoom-LOD fade (with_alpha) so the whole chip dissolves together.
        const ImU32 bg = on ? with_alpha(IM_COL32(30, 26, 18, hot ? 175 : 120))
                            : with_alpha(IM_COL32(46, 22, 22, hot ? 175 : 120));
        fg->AddRectFilled(r0, r1, bg, 4.f);
        if (hot)
            fg->AddRect(r0, r1, with_alpha(IM_COL32(238, 226, 188, 220)), 4.f, 0, 1.5f);

        const ImU32 col = on ? with_alpha(IM_COL32(238, 226, 188, 235))   // muted gold (on)
                             : with_alpha(IM_COL32(150, 140, 120, 160));  // dimmed (off)
        fg->AddText(font, fontSize, ImVec2(tp.x + 1.5f, tp.y + 1.5f), shadow, a.name);
        fg->AddText(font, fontSize, tp, col, a.name);
        if (!on) // strike-through = region hidden
            fg->AddLine(ImVec2(tp.x, p.y), ImVec2(tp.x + ts.x, p.y), col, 2.0f);
    }
}

// Quest-aware gating: a WorldQuestNPC marker is hidden while its questline is
// inactive (NONE of its quest-active flags is set). Mirrors the legacy native gate
// (refresh_quest_npc_eviction, goblin_inject.cpp). An NPC with no QuestGate entry is
// always shown. Gate join is by name_id (== QuestGate.nameId).
bool quest_npc_gated_out(const Marker &m)
{
    using namespace goblin::generated;
    if (!goblin::config::questNpcQuestAware) return false;
    if (m.category != (int)Category::WorldQuestNPC) return false;
    // Cold-API safety: until AlwaysOn (6001) reads true the flag manager isn't warm —
    // never blank every quest NPC on map open. Show as-is.
    if (!goblin::ui::read_event_flag(6001)) return false;
    for (size_t i = 0; i < QUEST_GATE_COUNT; ++i)
        if (QUEST_GATES[i].nameId == (uint32_t)m.name_id)
        {
            for (uint32_t f : QUEST_GATES[i].flags)
                if (f && goblin::ui::read_event_flag(f)) return false; // quest active → show
            return true; // gate found, none of its flags active → hide
        }
    return false; // no gate for this NPC → always show
}
} // namespace

bool inworld_hovered() { return s_inworld_hot; }

void set_grace_sprite(void *tex, float u0, float v0, float u1, float v1)
{
    s_grace_tex = reinterpret_cast<ImTextureID>(tex);
    s_grace_uv0 = ImVec2(u0, v0);
    s_grace_uv1 = ImVec2(u1, v1);
}

void set_grace_dungeon_sprite(void *tex, float u0, float v0, float u1, float v1)
{
    s_grace_dgn_tex = reinterpret_cast<ImTextureID>(tex);
    s_grace_dgn_uv0 = ImVec2(u0, v0);
    s_grace_dgn_uv1 = ImVec2(u1, v1);
}

void render_markers(const std::vector<MarkerLayer *> &layers, void *atlas_texture, float mouseX,
                    float mouseY)
{
    namespace proj = goblin::projection;
    const IconSet icons(reinterpret_cast<ImTextureID>(atlas_texture),
                        goblin::config::nativeItemIcons);
    const ImVec2 mouse(mouseX, mouseY);
    Hover hover;
    goblin::worldmap_probe::LiveView lv;
    if (!goblin::worldmap_probe::get_live_view(lv))
    {
        g_view_delay.reset(); // map closed → re-seed the delay fresh on reopen
        return;
    }

    // Per-frame worldmap render cost (aggregate-only — runs every frame the map is
    // open). Diagnoses whether the felt map lag is THIS render path vs the background
    // refresh thread (read_wgm). Shows as render.worldmap in the [BENCH] session report.
    GOBLIN_BENCH_QUIET("render.worldmap");

    ImGuiIO &io = ImGui::GetIO();
    // Background draw list (above the game map, BELOW the F1 ImGui window) so the F1 menu
    // stays on top of our markers instead of being covered by them.
    ImDrawList *fg = ImGui::GetBackgroundDrawList();
    const float realW = io.DisplaySize.x, realH = io.DisplaySize.y;

    proj::View view = to_proj_view(lv);

    // Which group's map is OPEN — from the SOLVED region getter (probe reads the
    // WorldMapDialog page+layer): openDlc = DLC map, underground = layer byte. The DLC
    // page has NO underground sub-layer (areas 40-43 share the one DLC page), so on the
    // DLC map we ignore the layer byte → DLC is always group 2. Base map keeps the
    // overworld/underground toggle. Computed BEFORE the motion-sync delay so a page swap
    // can snap it (below).
    const int open_grp = lv.openDlc ? 2 : ((lv.underground != 0) ? 1 : 0);

    // Page-transition snap: switching map page flips the native map CANVAS instantly — the
    // engine snaps pan/zoom between fixed per-page values, no ease (RE findings §7b). The
    // motion-sync delay below, tuned for smooth pans, would instead project markers from the
    // PREVIOUS frame's view for one frame → the OLD map flashes for 1 frame at the switch.
    // Drop the delay history (re-seed to the live, already-snapped view) on ANY page change,
    // not just the OW↔DLC↔UG group: key on the full page identity (group + open area
    // WorldMapArea+0x6e) so within-group page/area swaps snap too, AND on the engine's own
    // swap-request edge (dialog+0xA44) so even same-area sub-page tab swaps catch it. The
    // page id flips instantly at the swap (page_transition findings), so this re-seeds on the
    // exact snap frame. Normal pans (same page, no swap) keep the 1-frame delay.
    const long long page_key =
        (static_cast<long long>(open_grp) << 32) ^ static_cast<uint32_t>(lv.viewArea);
    static long long s_prev_page = -1;
    if (page_key != s_prev_page || lv.swapEdge)
        g_view_delay.reset();
    s_prev_page = page_key;

    // Motion sync: delay the projected view by the baked frame so markers ride the
    // native map layer instead of leading it during a pan.
    g_view_delay.apply(view, kViewDelayFrames);

    // Grace GPU offset rides the SAME projection as the markers (zoom × canvas factor), so the
    // native-vs-imgui calibration nudge stays aligned across zoom. Sampled here, after the delay
    // finalizes view.zoom; draw_marker reads s_grace_off_sx/sy. (kx/ky mirror project_screen.)
    s_grace_off_sx = view.zoom * (realW / 1920.f);
    s_grace_off_sy = view.zoom * (realH / 1080.f);
    s_grace_zoom = view.zoom;   // grace GPU icon scales with zoom (draw_marker reads this)

    // Clustering = a live render pass: categories opted into clustering bin together
    // into ONE mixed pile per dense screen cell; everything else draws normally. Live
    // by construction (re-binned every frame), so toggles/zoom update with no rebuild.
    const bool clustering = goblin::ui::clustering_enabled();
    const int threshold = clustering ? goblin::ui::global_threshold() : 0;
    // Distance-adaptive is its own clustering mode: when on it OVERRIDES the per-category
    // opt-in (every category clusters by distance from the player; see draw_clusters). Works
    // on ALL pages — the ramp measures distance in the RAW per-area frame (get_player_raw_pos
    // / grace_anchor_raw), gated to the same raw area, so the projected-frame overlap that
    // broke it underground (Siofra) no longer applies.
    const bool dist_adaptive =
        clustering && goblin::config::clusterDistanceAdaptive;
    // Resolution-relative icon/glyph sizes (match the native canvas-scaled icons),
    // × the user's master scale × the per-type scale (saved in the ini).
    const float uiScale = realH / 1080.f;
    const float master = goblin::config::overlayMasterScale;
    const float iconHalf = kIconHalfBase * uiScale * master * goblin::config::overlayIconScale;
    const float glyphR = kGlyphRBase * uiScale * master * goblin::config::overlayClusterScale;
    std::vector<ScreenMarker> clustered; // markers whose category opted into clustering

    // In-world region chips: project every major-region anchor once (shared by the chip
    // drawer + the per-marker region gate), then draw the clickable names. any_region_off
    // gates the marker loop's nearest-region test (skipped entirely when all-on).
    s_inworld_hot = false;
    compute_region_proj();
    const bool anyRegionOff = any_region_off();
    if (goblin::config::showRegionLabels)
        draw_region_labels(fg, open_grp, view, realW, realH, uiScale, ImVec2(mouseX, mouseY));

    // DEBUG (debug_region_volumes): draw every MapNameOverride region volume on the open
    // page at its projected centre + its name. RED = its textId does NOT resolve in the FMG
    // (the bug — region returns the id but lookup_text_utf8 gives nothing), cyan = resolves.
    if (goblin::config::debugRegionVolumes)
    {
        namespace gen = goblin::generated;
        for (size_t i = 0; i < gen::NAME_REGION_COUNT; ++i)
        {
            const auto &r = gen::NAME_REGIONS[i];
            int ga; float wx, wz;
            goblin::marker_world_pos(r.area, r.gx, r.gz, r.px, r.pz, ga, wx, wz,
                                     /*conv_underground=*/true);
            if (goblin::marker_group_from(r.area, ga) != open_grp)
                continue;
            float gU, gV;
            world_to_mapspace_xy(wx, wz, gU, gV);
            proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
            if (p.x < -4 || p.y < -4 || p.x > realW + 4 || p.y > realH + 4)
                continue;
            std::string nm = goblin::lookup_text_utf8(r.text_id);
            const ImU32 col = nm.empty() ? IM_COL32(255, 60, 60, 255)  // unresolved
                                         : IM_COL32(80, 200, 255, 255); // resolves
            fg->AddCircleFilled(ImVec2(p.x, p.y), 4.f, col);
            fg->AddCircle(ImVec2(p.x, p.y), 4.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
            char b[160];
            std::snprintf(b, sizeof(b), "%s [%d]", nm.empty() ? "UNRESOLVED" : nm.c_str(),
                          r.text_id);
            fg->AddText(ImVec2(p.x + 6, p.y - 5), col, b);
        }
    }

    // Batch pre-warm: project EVERY marker (ALL pages) the first time the live VM is ready,
    // not lazily per-page. The page gate below culls off-page markers BEFORE project_marker,
    // so without this each page's markers project on their FIRST view → a visible 1-by-1
    // pop-in on open / page-change. The projection is page-independent (static converters) →
    // valid forever once cached, so one upfront pass makes every reopen/page-switch instant.
    if (goblin::config::liveProjection)
    {
        static bool s_prewarmed = false;
        if (!s_prewarmed)
        {
            bool all_done = true;
            for (auto *L : layers)
            {
                if (!L)
                    continue;
                for (const Marker &m : L->markers())
                {
                    if (m.live_state == 1 || m.raw_area < 0)
                        continue;
                    float u, v;
                    int pg = -1;
                    if (goblin::worldmap_probe::project(m.raw_area, m.raw_gx, m.raw_gz, m.raw_px,
                                                        m.raw_pz, u, v, pg))
                    {
                        m.live_u = u;
                        m.live_v = v;
                        m.live_page = pg;
                        m.live_state = 1;
                    }
                    else
                        all_done = false; // VM not ready yet → retry next frame
                }
            }
            s_prewarmed = all_done;
        }
    }

    // Per-frame render-cost breakdown (aggregate-only — shows in the [BENCH] session report
    // as render.worldmap.markers = the main project+cull+gate+draw loop, render.worldmap.clusters
    // = the pile pass). Counters feed a throttled debug log so the felt lag at high marker counts
    // can be attributed (raw iteration vs actual draws). n_iter counts EVERY marker the loop
    // touches across visible categories (the true O(n) cost), before the page/cull gates.
    int n_iter = 0, n_drawn = 0, n_deferred = 0;
    {
    GOBLIN_BENCH_QUIET("render.worldmap.markers");
    for (auto *L : layers)
    {
        if (!L || !L->visible())
            continue;
        for (const Marker &m : L->markers())
        {
            ++n_iter;
            if (m.group != open_grp)
                continue; // draw only the open map group

            // PROJECT + CULL FIRST, gates LAST. The per-marker visibility gates below
            // (discover/quest/secondary/hide_when/fragment/fog) each call into the game's
            // event-flag lookup — cheap individually but run thousands of times per frame.
            // On a zoomed map most on-page markers are OFF-SCREEN, so doing the cheap
            // cached projection + screen cull BEFORE the gates skips all that work for them
            // (this was the dominant render.worldmap cost). Clustered-eligible markers are
            // NOT culled — an off-screen member still counts toward its grace pile/centroid
            // — but their eligibility doesn't depend on any flag, so we can test it early.
            const bool clustered_eligible =
                clustering && m.category >= 0 && m.cluster_key >= 0 &&
                (dist_adaptive || goblin::ui::category_clustered(m.category));

            float gU, gV;
            project_marker(m, gU, gV);
            proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
            ImVec2 sp(p.x, p.y);
            const bool offscreen =
                (sp.x < -32 || sp.y < -32 || sp.x > realW + 32 || sp.y > realH + 32);
            if (offscreen && !clustered_eligible)
                continue; // off-screen + not a pile member → no gates, no draw

            // In-world region toggle: hide markers whose nearest major-region anchor (same
            // group, map-space) is switched OFF. Skipped when all regions are on.
            if (anyRegionOff)
            {
                const int ri = nearest_region(m.group, gU, gV);
                if (ri >= 0 && !g_region_on[ri])
                    continue;
            }

            // ── visibility gates (now only for on-screen markers + all pile members) ──
            // Graces (discover_flag set only on grace markers). With grace_overlay the overlay draws
            // ALL graces itself (draw_marker: discovered = colour, undiscovered = grey). Without it,
            // the old hybrid: drop discovered graces (the game draws those natively), keep undiscovered.
            // Read live so it updates the moment the player rests at a grace.
            if (m.discover_flag && !goblin::config::graceOverlay &&
                goblin::ui::read_event_flag((uint32_t)m.discover_flag))
                continue;
            // Quest-aware gating: hide a quest-NPC whose questline is currently inactive.
            if (quest_npc_gated_out(m))
                continue;
            // Post-event story gate: a marker tagged with a secondary story flag (post-burn
            // Leyndell / Chapel, Ashen Capital, Charm-broken, Sealing-tree-burnt) is a
            // post-event variant and appears only once that flag is set. Read live so it
            // reveals the instant the event fires. Legacy parity: SetSecondaryFlags
            // (textEnableFlag2) + the Ashen Capital eventFlag gate.
            if (m.secondary_flag && !goblin::ui::read_event_flag((uint32_t)m.secondary_flag))
                continue;
            // Inverse story gate: a PRE-event variant (Leyndell Royal Capital) disappears
            // the moment its flag fires (the Ashen Capital replaces it).
            if (m.hide_when_flag && goblin::ui::read_event_flag((uint32_t)m.hide_when_flag))
                continue;
            // Map-fragment gate (require_map_fragments): the region's MAP FRAGMENT item must be
            // acquired (m.fragment_flag = GetMapFlagFromTile, read live).
            if (goblin::config::requireMapFragments && m.fragment_flag &&
                !goblin::ui::read_event_flag(static_cast<uint32_t>(m.fragment_flag)))
                continue; // map fragment not acquired yet

            // Clustered-eligible markers are deferred (uncull'd) to the pile pass;
            // everything else (already on-screen here) draws now.
            if (clustered_eligible)
            {
                clustered.push_back({sp, &m});
                ++n_deferred;
            }
            else
            {
                draw_marker(fg, m, sp, icons, iconHalf);
                hover_test(hover, mouse, sp, iconHalf, [&] { return marker_label(m); });
                ++n_drawn;
            }
        }
    }
    } // render.worldmap.markers

    {
        GOBLIN_BENCH_QUIET("render.worldmap.clusters");
        if (clustering && !clustered.empty())
            draw_clusters(fg, clustered, threshold, icons, realW, realH, view,
                          /*dist_eligible=*/true, iconHalf, glyphR, mouse, hover);
    }

    // Throttled cost attribution (debug_logging only; ~every 300 frames). Read with
    // grep "\[RENDERPERF\]" alongside the [BENCH] render.worldmap.* timings.
    if (goblin::config::debugLogging)
    {
        static int s_fc = 0;
        if (++s_fc % 300 == 0)
            spdlog::debug("[RENDERPERF] iterated={} drawn={} deferred(cluster)={} group={}",
                          n_iter, n_drawn, n_deferred, open_grp);
    }

    // Tooltip for the hovered marker / pile (drawn last so it's on top).
    if (hover.bestd < 1e30f)
        draw_tooltip(fg, mouse, hover.text);
}

void draw_minimap(const std::vector<MarkerLayer *> &layers, void *atlas_texture, float screenW,
                  float screenH)
{
    namespace cfg = goblin::config;
    if (!cfg::showMinimap || !goblin::ui::icons_enabled())
        return;
    // Hide the minimap while the full world map is open (it's a gameplay HUD).
    if (goblin::world_map_open())
        return;
    // Live player position (read during gameplay, map closed). The player pos is the
    // WorldMapPointParam frame on EVERY page now (RE windows_player_pos_RESOLVED), so the
    // minimap works overworld, underground AND DLC — pick the player's marker group.
    int parea = 0, pgroup = 0;
    float pwx = 0.f, pwz = 0.f;
    if (!goblin::get_player_map_pos(parea, pwx, pwz, nullptr, nullptr, &pgroup))
        return; // no position (e.g. during a load) → no minimap this frame

    const float R = cfg::minimapSize > 24.f ? cfg::minimapSize : 24.f;
    const float scale = cfg::minimapZoom > 0.0001f ? cfg::minimapZoom : 0.08f;
    const float margin = 24.f;
    // Configurable corner + pixel offset.
    const float ax = cfg::minimapAnchorRight ? (screenW - R - margin - cfg::minimapOffsetX)
                                             : (R + margin + cfg::minimapOffsetX);
    const float ay = cfg::minimapAnchorBottom ? (screenH - R - margin - cfg::minimapOffsetY)
                                              : (R + margin + cfg::minimapOffsetY);
    const ImVec2 ctr(ax, ay);
    const float cullR = R - 5.f;

    int bgA = (int)(cfg::minimapOpacity * 255.f);
    bgA = bgA < 0 ? 0 : (bgA > 255 ? 255 : bgA);

    ImDrawList *fg = ImGui::GetBackgroundDrawList(); // below the F1 window (topmost)
    fg->AddCircleFilled(ctr, R, IM_COL32(12, 14, 20, bgA), 64);
    fg->AddCircle(ctr, R, IM_COL32(230, 220, 180, 200), 64, 2.0f);

    fg->PushClipRect(ImVec2(ctr.x - R, ctr.y - R), ImVec2(ctr.x + R, ctr.y + R), true);
    const IconSet icons(reinterpret_cast<ImTextureID>(atlas_texture),
                        goblin::config::nativeItemIcons);
    const float half = 6.0f; // minimap markers are small + fixed-size
    for (auto *L : layers)
    {
        if (!L || !L->visible())
            continue;
        for (const Marker &m : L->markers())
        {
            if (m.group != pgroup)
                continue; // only the player's current map page
            // Same hide-gates as the worldmap (discovered grace, quest-NPC, post-event story).
            if (m.discover_flag && !goblin::config::graceOverlay &&
                goblin::ui::read_event_flag((uint32_t)m.discover_flag))
                continue;
            if (quest_npc_gated_out(m))
                continue;
            if (m.secondary_flag && !goblin::ui::read_event_flag((uint32_t)m.secondary_flag))
                continue;
            // Inverse story gate: a PRE-event variant (Leyndell Royal Capital) disappears
            // the moment its flag fires (the Ashen Capital replaces it).
            if (m.hide_when_flag && goblin::ui::read_event_flag((uint32_t)m.hide_when_flag))
                continue;
            // North-up, player-centred: same orientation as the worldmap (mapV = -worldZ).
            float dx = (m.worldX - pwx) * scale;
            float dy = -(m.worldZ - pwz) * scale;
            if (dx * dx + dy * dy > cullR * cullR)
                continue; // outside the HUD radius
            // Map-fragment gate (require_map_fragments): the FRAGMENT item must be acquired.
            if (cfg::requireMapFragments && m.fragment_flag &&
                !goblin::ui::read_event_flag(static_cast<uint32_t>(m.fragment_flag)))
                continue;
            draw_marker(fg, m, ImVec2(ctr.x + dx, ctr.y + dy), icons, half);
        }
    }
    fg->PopClipRect();

    // Player marker at centre + a north tick (no heading yet → north-up only).
    fg->AddCircleFilled(ctr, 4.0f, IM_COL32(255, 225, 70, 255));
    fg->AddCircle(ctr, 4.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    fg->AddText(ImVec2(ctr.x - 4.f, ctr.y - R - 16.f), IM_COL32(230, 220, 180, 220), "N");
}
} // namespace goblin::worldmap
