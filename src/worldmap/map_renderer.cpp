#include "map_renderer.hpp"
#include "category_meta.hpp"          // category_gpu_iconId (map-point symbol per category)
#include "spatial_grid.hpp"           // tile-based clustering (grid_cell_key)

#include "goblin_bench.hpp"          // GOBLIN_BENCH_QUIET (per-frame render timing)
#include "goblin_projection.hpp"     // baked map-space → backbuffer projection
#include "goblin_worldmap_probe.hpp" // get_live_view()
#include "goblin_inject.hpp"         // ui::clustering_enabled / global_threshold / category_clustered
#include "goblin_overlay.hpp"        // overlay::native_map_point_icon (native GPU map-symbol harvest)
#include "goblin_config.hpp"         // overlay marker scale config
#include "goblin_messages.hpp"       // lookup_text_utf8 (tooltip names)
#include "goblin_collected.hpp"      // is_original_row_collected (rune/ember graying)
#include "goblin_kindling.hpp"       // is_row_collected (kindling graying)
#include "goblin_major_regions.hpp"  // MAJOR_REGION_ANCHORS (region labels)
#include "goblin_name_regions.hpp"   // NAME_REGIONS (MapNameOverride debug viz)
#include "goblin_quest_gates.hpp"    // QUEST_GATES (quest-NPC gating)
#include "goblin_map_data.hpp"       // Category enum (WorldQuestNPC)
#include "goblin_markers.hpp"        // markers::category_name (readable [ICONTIER] audit output)

#include <chrono>
#include <string>
#include <unordered_set>
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
// Baked: the native GFx map layer is composited ~1 frame behind our Present sample, so map-bound
// markers need a view delay to ride the map. The delay amount is the live config view_delay_frames
// (default 1.0) — A/B-tunable in-game to fix the pan/zoom marker re-adjust; see g_view_delay.apply.
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
    enum Source { Atlas, MapPoint } source = Atlas;
    const char *atlas_key = nullptr; // Atlas: category cell key ("show_bosses")
    int icon_id = -1;                // MapPoint: numeric game IconId (MENU_MAP_<NN>)
};

// Which tier of the provider chain supplied a glyph. Drives the [ICONTIER] audit census so we can
// see — per mod (ERR vs vanilla) — exactly which categories rely on the baked atlas or fall to a
// circle, i.e. what removing the baked atlas would regress.
enum IconTier
{
    TIER_MP_NAME = 0, // native map-point symbol by ERR name (category_gpu_icon_name)
    TIER_MP_ID,       // native map-point symbol by numeric id (category_gpu_iconId, e.g. 89)
    TIER_ITEM,        // per-item inventory icon (native_item_icon, m.item_icon_id)
    TIER_REP,         // category representative inventory icon (category_rep_icon)
    TIER_ATLAS,       // baked overlay atlas (the transitional fallback we want to drop)
    TIER_CIRCLE,      // no glyph resolved → plain circle (universal fallback)
    TIER_COUNT
};

struct IconHandle
{
    ImTextureID tex = nullptr;
    ImVec2 uv0{}, uv1{};
    float scale = 1.0f; // draw-size multiplier (native map symbols are bigger than item dots)
    int tier = TIER_CIRCLE; // tier of the provider chain that resolved this handle (audit census)
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
        out.tier = TIER_ATLAS;
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
        {
            // Resident GPU symbol not loaded (map closed, or this mod has no such symbol resident) ->
            // mod-agnostic DISK glyph by iconId (same no-bake path as the undiscovered-grace render).
            if (!goblin::overlay::map_point_glyph_uv(nullptr, k.icon_id, tex, u0, v0, u1, v1))
                return false;
        }
        out.tex = reinterpret_cast<ImTextureID>(tex);
        out.uv0 = ImVec2(u0, v0);
        out.uv1 = ImVec2(u1, v1);
        return true;
    }
};

// The active provider chain + per-marker resolution policy: native map-point symbol FIRST (for
// the categories that have a real game symbol via category_gpu_iconId), then the baked category
// atlas (guaranteed coverage), else the caller draws a circle. One instance per render pass.
struct IconSet
{
    AtlasProvider atlas;
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
                    // Per-category multiplier: normal hostile entities reuse the boss symbol smaller.
                    out.scale = goblin::config::mapSymbolScale *
                                goblin::worldmap::category_gpu_icon_scale(m.category);
                    out.tier = TIER_MP_NAME;
                    return true;
                }
            }
            int gid = goblin::worldmap::category_gpu_iconId(m.category);
            if (gid > 0 && mappoint.resolve(IconKey{IconKey::MapPoint, nullptr, gid}, out))
            {
                out.scale = goblin::config::mapSymbolScale;
                out.tier = TIER_MP_ID;
                return true;
            }
            // Per-item: this loot's OWN inventory icon (resolved at build from the live lot, stored on
            // the marker). Preferred over the category representative so each drop shows its real icon.
            // native_item_icon is resident-GPU-then-disk; on a miss we fall through to the rep below.
            if (m.item_icon_id > 0)
            {
                void *t = nullptr; float a0, b0, a1, b1;
                if (goblin::overlay::native_item_icon(m.item_icon_id, t, a0, b0, a1, b1))
                {
                    out.tex = reinterpret_cast<ImTextureID>(t);
                    out.uv0 = ImVec2(a0, b0); out.uv1 = ImVec2(a1, b1);
                    out.tier = TIER_ITEM;
                    return true;
                }
            }
            // Item categories → the game's real inventory icon for a representative member
            // (category_rep_icon, derived live at build). Harvested from the 00_Solo atlas; falls
            // through to the baked atlas until that icon is resident.
            int rep = goblin::worldmap::category_rep_icon(m.category);
            if (rep > 0)
            {
                void *t = nullptr; float a0, b0, a1, b1;
                if (goblin::overlay::native_item_icon(rep, t, a0, b0, a1, b1))
                {
                    out.tex = reinterpret_cast<ImTextureID>(t);
                    out.uv0 = ImVec2(a0, b0); out.uv1 = ImVec2(a1, b1);
                    out.tier = TIER_REP;
                    return true;
                }
            }
        }
        return atlas.resolve(IconKey{IconKey::Atlas, m.icon_key, -1}, out);
    }
};

// ---- [ICONTIER] audit census -------------------------------------------------
// Counts how many drawn markers each tier of the chain resolved, and records WHICH categories fell
// through to the baked atlas or to a plain circle. Run the game in ERR, then in a vanilla profile
// (ModEngine3 CLI, custom path), grep "[ICONTIER]" in both logs, and diff: any category that is
// native/rep in ERR but atlas/circle in vanilla is what dropping the baked atlas would regress.
// Throttled to one line every few seconds so it never spams the per-frame draw path.
struct TierCensus
{
    long counts[TIER_COUNT] = {0};
    std::unordered_set<int> atlas_cats;  // categories that resolved via the baked atlas
    std::unordered_set<int> circle_cats; // categories that fell all the way to a circle
};
static TierCensus g_tier_census;
static std::chrono::steady_clock::time_point g_tier_last_log{};

static void tier_tally(int tier, int category)
{
    if (tier < 0 || tier >= TIER_COUNT)
        return;
    g_tier_census.counts[tier]++;
    if (tier == TIER_ATLAS)
        g_tier_census.atlas_cats.insert(category);
    else if (tier == TIER_CIRCLE)
        g_tier_census.circle_cats.insert(category);
}

static std::string tier_cat_list(const std::unordered_set<int> &cats)
{
    if (cats.empty())
        return "-";
    std::string s;
    for (int c : cats)
    {
        if (!s.empty())
            s += ", ";
        s += goblin::markers::category_name(static_cast<goblin::generated::Category>(c));
    }
    return s;
}

// Flush a throttled summary. Called once per render pass; emits at most one line every ~3s, then
// resets the accumulator so each line reflects a fresh window of frames.
static void tier_census_flush()
{
    long total = 0;
    for (int i = 0; i < TIER_COUNT; ++i)
        total += g_tier_census.counts[i];
    if (total == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (g_tier_last_log.time_since_epoch().count() != 0 &&
        now - g_tier_last_log < std::chrono::seconds(3))
        return;
    g_tier_last_log = now;
    spdlog::info("[ICONTIER] mp_name={} mp_id={} item={} rep={} atlas={} circle={} (total={})",
                 g_tier_census.counts[TIER_MP_NAME], g_tier_census.counts[TIER_MP_ID],
                 g_tier_census.counts[TIER_ITEM], g_tier_census.counts[TIER_REP],
                 g_tier_census.counts[TIER_ATLAS], g_tier_census.counts[TIER_CIRCLE], total);
    spdlog::info("[ICONTIER]   atlas-only categories : {}", tier_cat_list(g_tier_census.atlas_cats));
    spdlog::info("[ICONTIER]   circle categories     : {}", tier_cat_list(g_tier_census.circle_cats));
    g_tier_census = TierCensus{};
}

// DX item 1 (legibility): draw a marker icon centered at `center` with a minimum on-screen size and
// (for small inventory-style icons only) a dark backing disc, so they don't vanish into the map art.
//
// `backing` MUST be false for native map symbols (tiers mp_name / mp_id — boss/grace/summon MENU_MAP_*):
// those are full map glyphs authored to read on the map at their own scale, and a disc behind them is
// just an ugly halo. Pass true only for the small item/rep inventory icons that actually blend in.
// `minHalf` is a harmless floor (native symbols are already larger, so it rarely triggers for them).
// Honors config::iconLegibility (off ⇒ raw image, original behaviour). One filled circle — no DDS touch.
static inline void draw_legible_icon(ImDrawList *fg, ImVec2 center, float half, ImTextureID tex,
                                     ImVec2 uv0, ImVec2 uv1, ImU32 tint, float minHalf, bool backing)
{
    if (goblin::config::iconLegibility)
    {
        // Only the genuinely SMALL icons blend into the map and need a disc; a big item icon reads on
        // its own, so backing it just darkens the map. Decide on the icon's natural (pre-clamp) size.
        const bool small = half < minHalf * 1.6f;
        if (half < minHalf)
            half = minHalf;
        if (backing && small)
            fg->AddCircleFilled(center, half + 1.5f, IM_COL32(0, 0, 0, 165)); // contrast backing
    }
    fg->AddImage(tex, ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                 uv0, uv1, tint);
}

// Backing only for the small inventory-style icons (item / category-rep), never native map symbols.
static inline bool tier_wants_backing(int tier) { return tier == TIER_ITEM || tier == TIER_REP; }

// DX item 7 (altitude cue): player world-Y cached once per render pass (block-local marker Y ≈ world Y
// on the overworld, so the diff is meaningful there; legacy-dungeon Y offset is a known caveat).
static float g_player_world_y = 0.0f;
static bool  g_player_world_y_valid = false;
static int   g_player_group = -1;   // player's current map layer (base/DLC × over/under); -1 = unknown

// Small ▲ (above) / ▼ (below) triangle in the marker's top-right corner when it sits well above/below
// its altitude REFERENCE. Drawn as primitives (AddTriangleFilled) — no font dependency, can't tofu.
// worldY==0 = no altitude data → skip. Reference:
//   • on the player's current page → the PLAYER (same Y frame); warm = above / cool = below.
//   • on any OTHER page → the nearest grace in the marker's own area (has_ref_grace; the player Y is in
//     a different frame there). DISTINCT tint (green above / teal below) so the changed reference reads
//     at a glance. No same-area grace → no badge. See offpage_altitude_via_grace_plan.md.
static inline void draw_altitude_badge(ImDrawList *fg, ImVec2 center, float half, const Marker &m)
{
    if (!goblin::config::altitudeCue || m.worldY == 0.0f)
        return;
    float d;
    bool grace_ref;
    if (g_player_world_y_valid && g_player_group >= 0 && m.group == g_player_group)
    {
        d = m.worldY - g_player_world_y;   // on the player's page → relative to the player
        grace_ref = false;
    }
    else if (m.has_ref_grace)
    {
        d = m.worldY - m.ref_grace_y;      // off-page → relative to the nearest same-area grace
        grace_ref = true;
    }
    else
        return;                            // off-page with no grace reference → nothing meaningful
    if (d > -goblin::config::altitudeDeadzone && d < goblin::config::altitudeDeadzone)
        return; // same level
    const bool above = d > 0.0f;
    const float s = 4.0f;                              // half-size of the triangle
    const float cx = center.x + half;                 // top-right corner anchor
    const float cy = center.y - half;
    const ImU32 fill = grace_ref
                           ? (above ? IM_COL32(140, 230, 120, 255) : IM_COL32(90, 200, 180, 255))
                           : (above ? IM_COL32(255, 205, 90, 255) : IM_COL32(110, 185, 255, 255));
    const ImU32 line = IM_COL32(0, 0, 0, 220);
    ImVec2 a, b, c;
    if (above) { a = ImVec2(cx, cy - s); b = ImVec2(cx - s, cy + s); c = ImVec2(cx + s, cy + s); }
    else       { a = ImVec2(cx, cy + s); b = ImVec2(cx - s, cy - s); c = ImVec2(cx + s, cy - s); }
    fg->AddTriangleFilled(a, b, c, fill);
    fg->AddTriangle(a, b, c, line, 1.0f);
}

// Desaturate toward luminance + halve alpha → the "collected" dim tint (packed ABGR).
unsigned int dim_color(unsigned int abgr)
{
    int a = (abgr >> 24) & 0xff, b = (abgr >> 16) & 0xff, g = (abgr >> 8) & 0xff, r = abgr & 0xff;
    int lum = (r * 54 + g * 183 + b * 19) >> 8; // ITU-R-ish luma
    r = (r + lum * 2) / 3; g = (g + lum * 2) / 3; b = (b + lum * 2) / 3;
    a /= 2;
    return ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}

// Is a single loot member collected? Same predicate as marker_done's loot branch (event flag +
// geom/kindling tracking), factored so an item-stack can test each absorbed member.
static bool loot_member_collected(uint64_t row_id, int collected_flag)
{
    if (collected_flag && goblin::ui::read_event_flag((uint32_t)collected_flag))
        return true;
    if (row_id && (goblin::collected::is_original_row_collected(row_id) ||
                   goblin::kindling::is_row_collected(row_id)))
        return true;
    return false;
}

// Total item count of an item-stack representative: Σ member.count over ALL members. For an
// unstacked marker (stacked empty) returns its own count.
int stacked_total_count(const Marker &m)
{
    if (m.stacked.empty())
        return m.count;
    int t = 0;
    for (const StackedMember &sm : m.stacked)
        t += sm.count;
    return t;
}

// Remaining (uncollected) item count of an item-stack representative: Σ member.count over members
// not yet collected. For an unstacked marker (stacked empty) returns its own count.
int stacked_remaining_count(const Marker &m)
{
    if (m.stacked.empty())
        return m.count;
    int rem = 0;
    for (const StackedMember &sm : m.stacked)
        if (!loot_member_collected(sm.row_id, sm.collected_flag))
            rem += sm.count;
    return rem;
}

// True when this marker should be treated as a STACK representative right now: it has merged members
// AND the stack toggle is on. When the toggle is off, a representative behaves as its own plain marker
// (and its members draw individually), so stacking adds/removes nothing without a rebuild.
static inline bool is_active_stack(const Marker &m)
{
    return goblin::config::stackIdenticalItems && !m.stacked.empty();
}

// Is this marker's item collected / boss cleared? cleared_only reports the boss-clear
// sub-case (gets a checkmark). Event flags (cleared/loot) read the live game state;
// rune/ember/kindling use the collected-tracking sets (refreshed by the mod thread).
// An ACTIVE item-stack is "done" only when EVERY merged member is collected; with the toggle off it
// falls through to the plain single-marker check (its members are drawn/judged individually).
bool marker_done(const Marker &m, bool &cleared_only)
{
    cleared_only = m.cleared_flag && goblin::ui::read_event_flag((uint32_t)m.cleared_flag);
    if (cleared_only)
        return true;
    if (is_active_stack(m))
    {
        for (const StackedMember &sm : m.stacked)
            if (!loot_member_collected(sm.row_id, sm.collected_flag))
                return false;
        return true;
    }
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

// Vanilla parity: a grace the player has already DISCOVERED (rested at) shows on the
// world map regardless of map-fragment ownership — the base game reveals lit graces even
// without the region's fragment. Only grace markers carry discover_flag, so this is false
// for every other category. Used to let discovered graces skip the require_fragment gate
// (undiscovered graces stay gated). Read live so it flips the instant the player rests.
static inline bool is_discovered_grace(const Marker &m)
{
    return m.discover_flag && goblin::ui::read_event_flag(static_cast<uint32_t>(m.discover_flag));
}

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
            // UNDISCOVERED grace → mod-agnostic DISK glyph (gold effigy MENU_MAP_Player_02 from the
            // active SB_MapCursor) instead of the bonfire sprite. Falls back to the sprite until the
            // DDS is read+uploaded, so nothing regresses. Discovered graces keep the sprite + check.
            if (!disc)
            {
                void *ut = nullptr; float gu0, gv0, gu1, gv1;
                if (goblin::overlay::map_point_glyph_uv("MENU_MAP_Player_02", -1, ut, gu0, gv0, gu1, gv1))
                {
                    gt = (ImTextureID)ut;
                    u0 = ImVec2(gu0, gv0);
                    u1 = ImVec2(gu1, gv1);
                    static bool s_logged = false;
                    if (!s_logged)
                    {
                        s_logged = true;
                        spdlog::info("[GRACEUNDISC] undiscovered grace -> MENU_MAP_Player_02 disk glyph "
                                     "tex={} uv=({},{})-({},{})", ut, gu0, gv0, gu1, gv1);
                    }
                }
            }
            draw_legible_icon(fg, ImVec2(gx, gy), gh, gt, u0, u1, t,
                              goblin::config::iconMinHalfPx > 10.0f ? goblin::config::iconMinHalfPx : 10.0f,
                              /*backing=*/false); // grace = native MENU_MAP symbol, no halo
            // No discovered-check on graces: the icon already encodes it (undiscovered = yellow
            // MENU_MAP_Player_02 cursor, discovered = the gold effigy sprite), so a check is redundant.
            return;
        }
        IconHandle ih;
        if (icons.resolve(m, ih))
        {
            tier_tally(ih.tier, m.category);
            draw_legible_icon(fg, p, half, ih.tex, ih.uv0, ih.uv1, t, goblin::config::iconMinHalfPx,
                              tier_wants_backing(ih.tier));
        }
        else
        {
            tier_tally(TIER_CIRCLE, m.category);
            float cr = half * 0.45f;
            fg->AddCircleFilled(p, cr, disc ? m.color : dim_color(m.color));
            fg->AddCircle(p, cr, IM_COL32(0, 0, 0, 220), 0, 1.5f);
        }
        draw_altitude_badge(fg, p, half, m);
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
        tier_tally(ih.tier, m.category);
        const float hh = half * ih.scale;
        draw_legible_icon(fg, p, hh, ih.tex, ih.uv0, ih.uv1, tint, goblin::config::iconMinHalfPx,
                          tier_wants_backing(ih.tier));
    }
    else
    {
        tier_tally(TIER_CIRCLE, m.category);
        float cr = half * 0.45f;
        const ImU32 fill = done ? dim_color(m.color) : (red ? IM_COL32(235, 70, 70, 255) : m.color);
        fg->AddCircleFilled(p, cr, fill);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, done ? 120 : 220), 0, 1.5f);
    }
    if (cleared)
        draw_check(fg, p, half);
    draw_altitude_badge(fg, p, half, m);
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
    ImVec2 p;        // screen position
    const Marker *m;
    float u = 0, v = 0;  // map-space position (for tile clustering)
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
    // " xN" quantity suffix: a multi-item lot, or an ACTIVE item-stack of co-located identical markers
    // (only when the stack toggle is on — off, a representative shows its own count like any marker).
    // For an active stack with collected_graying on, show the REMAINING (uncollected) count so it
    // depletes as nodes are gathered (xN→…→1); otherwise the stack total. ASCII 'x' (not '×') so it
    // can't tofu on a font missing the multiply glyph. Empty for single items.
    int shown = m.count;
    if (is_active_stack(m))
        shown = goblin::config::collectedGraying ? stacked_remaining_count(m) : stacked_total_count(m);
    const std::string qty = (shown > 1) ? (" x" + std::to_string(shown)) : std::string();
    // Spoiler-free: don't leak the item name — just "?" (+ its location, like native). Quantity is
    // not an identity spoiler, so it still shows.
    if (goblin::config::anonymousLoot && m.lot_backed)
        return loc.empty() ? ("?" + qty) : ("?" + qty + "\n" + loc);
    std::string name = goblin::lookup_text_utf8(m.name_id);
    if (name.empty())
    {
        // The FMG name resolved to nothing — common for ERR-custom loot whose live EquipParam ICON
        // resolves but whose name string isn't in the (vanilla-derived) preloaded FMG. For a loot/stack
        // marker, show a placeholder so it still has a label AND keeps its " xN" count (returning just
        // the location here used to silently drop the stack count). Non-loot nameless markers are
        // unchanged (location only).
        if (m.lot_backed || !m.stacked.empty() || !qty.empty())
            name = "Unknown item";
        else
            return loc;
    }
    name += qty;
    if (loc.empty()) return name;
    return name + "\n" + loc; // item name (+ qty), then its location on the next line
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
    // Tile clustering: group by the marker's MAP-SPACE tile (+ map group), not the nearest-grace key.
    // Deterministic + zoom-aware, and every group is bounded to one 256-unit tile, so a pile's centroid
    // can't drift across the map the way the old grace-key centroid fallback could.
    std::unordered_map<uint32_t, std::vector<int>> groups; // tile cell key → member indices
    groups.reserve(items.size());
    for (int i = 0; i < (int)items.size(); ++i)
        groups[grid_cell_key(items[i].m->group, items[i].u, items[i].v)].push_back(i);

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
    // Tile clustering measures player↔tile in MAP-SPACE, gated to the player's own map layer (group).
    // Group-keyed tiles never share a cell across layers, so this avoids the overworld↔underground
    // overlap without needing the raw-area frame the old grace-key ramp relied on.
    int player_grp = -1;
    float pmU = 0, pmV = 0;
    if (have_player)
    {
        int pgx_ = 0, pgz_ = 0;
        goblin::get_player_map_pos(player_area, pwx, pwz, &pgx_, &pgz_, &player_grp);
        world_to_mapspace_xy(pwx, pwz, pmU, pmV);
    }
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
        // Tile centroid in MAP-SPACE. All members share one 256-unit tile, so this is a tight, bounded
        // anchor for both the threshold distance and the pile placement — no cross-map drift.
        float cu = 0, cv = 0;
        for (int i : idxs) { cu += items[i].u; cv += items[i].v; }
        cu /= (float)idxs.size(); cv /= (float)idxs.size();
        // Per-tile threshold. Distance-adaptive: Euclidean ramp near_thr→base_thr over the near→far
        // radius, measured in MAP-SPACE and gated to the player's own map layer (the tile's group, from
        // the high bits of the cell key, must equal the player's group). Otherwise flat base_thr.
        // The size threshold is an ADAPTIVE-only knob: with plain clustering, any tile holding more than
        // one marker piles (thr=1). With distance-adaptive on, thr starts at base_thr and ramps below.
        int thr = dist_adaptive ? base_thr : 1;
        float dbg_d = -1.f; // map-space distance player→tile (for the debug viz)
        const int tile_grp = (int)((kv.first >> 28) & 0xF);
        if (have_player && tile_grp == player_grp && far_u > near_u)
        {
            const float du = cu - pmU, dv = cv - pmV;
            const float d = std::sqrt(du * du + dv * dv);
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
        // Pile at the member SCREEN CENTROID. Members are bounded to one map tile, so the centroid is
        // tight and central — no cross-map drift (the reason the old grace-anchored placement existed).
        ImVec2 c;
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
            const ImU32 dcol = IM_COL32(40, 230, 170, 170); // tile pile = centroid placed
            for (int i : idxs)
                fg->AddLine(c, items[i].p, dcol, 1.0f);
            fg->AddCircleFilled(c, 4.f, dcol);
            fg->AddCircle(c, 4.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
            std::string nm = goblin::lookup_text_utf8(items[idxs[0]].m->loc_pname);
            char db[140];
            if (dbg_d >= 0.f) // distance-adaptive engaged: show dist + chosen threshold
                std::snprintf(db, sizeof(db), "%s [%d] d=%.0f thr=%d",
                              nm.c_str(), (int)idxs.size(), dbg_d, thr);
            else
                std::snprintf(db, sizeof(db), "%s [%d] thr=%d",
                              nm.c_str(), (int)idxs.size(), thr);
            fg->AddText(ImVec2(c.x + 7, c.y + 5), dcol, db);
        }
        // Pile count = UNCOLLECTED members (depletion), so the glyph reflects progress
        // instead of the static total. When collected_graying is off, show the full
        // total. marker_done = the same collected/cleared predicate used for graying.
        // This counts MARKERS (one per lot), not items — per-lot quantity lives in each
        // marker's hover tooltip (Marker.count), not the pile glyph.
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

// ── Item search highlight (F1 search bar) ───────────────────────────────────────────────────────
// The overlay resolves which marker name_ids match the search query (rebuilt only when the query
// changes) and hands a pointer to that set here each frame; render_markers rings those markers and
// pulls them out of any cluster pile so each is individually visible. A "locate" request (a clicked
// result) latches a name_id; the loop captures that marker's backbuffer screen pos so the overlay can
// point the OS cursor at it (cursor = the 2D map camera). All cheap: one set lookup per marker.
static const std::unordered_set<int32_t> *s_search_set = nullptr;
static int32_t s_locate_nameid = 0;     // latched until the loop finds it
static bool s_locate_have = false;      // a captured marker-space coord is waiting for the overlay
static ImVec2 s_locate_pos{};           // (gU, gV) marker space of the located marker

void set_item_search(const std::unordered_set<int32_t> *matchNameIds, int32_t locateNameId)
{
    const bool active = matchNameIds && !matchNameIds->empty();
    s_search_set = active ? matchNameIds : nullptr;
    if (locateNameId) s_locate_nameid = locateNameId;
    else if (!active) s_locate_nameid = 0; // search cleared → cancel any pending locate
}

bool item_search_active() { return s_search_set != nullptr; }

// A locate is still WAITING for its marker — the clicked result is on a page that isn't open yet, so
// the loop hasn't found it. Lets the panel show "switch to that page; it'll centre automatically".
bool locate_pending() { return s_locate_nameid != 0; }

bool take_locate_pos(float *u, float *v)
{
    if (!s_locate_have) return false;
    s_locate_have = false;
    if (u) *u = s_locate_pos.x;
    if (v) *v = s_locate_pos.y;
    return true;
}

static inline bool search_hit(const Marker &m)
{
    return s_search_set && m.name_id >= 0 && s_search_set->count(m.name_id) != 0;
}

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

    // DX item 7: cache the player's world-Y + map GROUP once for this pass. The altitude badge is only
    // meaningful when the marker shares the player's coordinate frame, i.e. the SAME map layer (base/DLC
    // × overworld/underground). When you view a different map than where the player physically is, the
    // Y values are in unrelated frames (everything would read "below"), so we gate on group match.
    {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        g_player_world_y_valid = goblin::get_player_world_pos(px, py, pz);
        g_player_world_y = py;
        int parea = 0, pgrp = -1;
        float mwx = 0.0f, mwz = 0.0f;
        g_player_group = goblin::get_player_map_pos(parea, mwx, mwz, nullptr, nullptr, &pgrp) ? pgrp : -1;
    }

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

    // Motion sync: delay the projected view by the configured frame count so markers ride the
    // native map layer instead of leading it during a pan. Live config (view_delay_frames) so the
    // pan/zoom re-adjust can be A/B-tuned in-game; apply() clamps to the ring's [0, N-1] capacity.
    g_view_delay.apply(view, goblin::config::viewDelayFrames, goblin::config::viewDelayZoom);

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
    // Master-off / hidden categories normally skip drawing — but an active item search REVEALS its
    // hits regardless (else "hide everything then search" finds nothing). master_on folds the icon
    // master into the per-layer visibility; a hidden layer is still iterated while searching so its
    // hits can draw, and non-hit markers in it stay hidden (the !layer_vis && !is_hit skip below).
    const bool master_on = goblin::ui::icons_enabled();
    // Locate-candidate selection: a searched name can have many markers across the page. Pick the
    // best one to pan onto — UNCOLLECTED first (the useful ones), then nearest to the current view
    // centre. If every instance is already collected, the nearest collected one wins (it draws greyed,
    // so the user sees it's done). View centre in marker space = (pan + snapMid)/zoom.
    const float locCenU = (view.panX + view.snapMidX) / view.zoom;
    const float locCenV = (view.panZ + view.snapMidZ) / view.zoom;
    bool loc_have = false, loc_best_uncollected = false;
    float loc_best_d = 1e30f;
    ImVec2 loc_best{};

    // Viewport in MAP-SPACE: unproject the 4 screen corners → bounding rect, expanded by one tile.
    // Clustered-eligible markers are not screen-culled (an off-screen member counts toward its pile),
    // so on a zoomed-in map they otherwise pay the expensive per-marker visibility gates for the whole
    // page every frame (the dominant render.worldmap.markers cost). A clustered marker whose map-space
    // lies outside this rect has an OFF-SCREEN pile cell → skip its gates. The one-tile margin keeps
    // every member of a cell that straddles the screen edge, so on-screen piles/centroids are intact.
    float vMinU = 1e30f, vMinV = 1e30f, vMaxU = -1e30f, vMaxV = -1e30f;
    {
        const float cx[4] = {0.f, realW, 0.f, realW}, cy[4] = {0.f, 0.f, realH, realH};
        for (int i = 0; i < 4; ++i)
        {
            float u, v;
            proj::unproject_screen(cx[i], cy[i], view, realW, realH, u, v);
            vMinU = std::min(vMinU, u); vMaxU = std::max(vMaxU, u);
            vMinV = std::min(vMinV, v); vMaxV = std::max(vMaxV, v);
        }
        const float margin = goblin::worldmap::kTileSize;
        vMinU -= margin; vMinV -= margin; vMaxU += margin; vMaxV += margin;
    }

    for (auto *L : layers)
    {
        if (!L)
            continue;
        const bool layer_vis = master_on && L->visible();
        if (!layer_vis && !s_search_set)
            continue; // hidden layer + not searching → skip entirely
        for (const Marker &m : L->markers())
        {
            ++n_iter;
            if (m.group != open_grp)
                continue; // draw only the open map group

            // Item stacking (render-time, instant toggle): a non-representative member of a co-located
            // identical-item group is hidden while the toggle is on — its representative draws once for
            // the whole group with the summed " xN". Toggle off → fall through and draw it individually.
            if (m.stack_member && goblin::config::stackIdenticalItems)
                continue;

            // [diag] baked-only filter: show ONLY surviving Baked-source markers (= the no-bake
            // residual; disk/live twins were already dropped at finalize). Lets each spot be
            // eyeballed in-world — real loot the live pass misses vs a phantom the bake invented.
            if (goblin::config::bakedOnly && m.source != Source::Baked)
                continue;

            // In a hidden layer (category off / master off) only an active search HIT draws.
            const bool is_hit = search_hit(m);
            if (!layer_vis && !is_hit)
                continue;

            // PROJECT + CULL FIRST, gates LAST. The per-marker visibility gates below
            // (discover/quest/secondary/hide_when/fragment/fog) each call into the game's
            // event-flag lookup — cheap individually but run thousands of times per frame.
            // On a zoomed map most on-page markers are OFF-SCREEN, so doing the cheap
            // cached projection + screen cull BEFORE the gates skips all that work for them
            // (this was the dominant render.worldmap cost). Clustered-eligible markers are
            // NOT culled — an off-screen member still counts toward its grace pile/centroid
            // — but their eligibility doesn't depend on any flag, so we can test it early.
            // A search hit is pulled OUT of any cluster pile so each match draws individually
            // (and gets its highlight ring), instead of hiding inside a location pile.
            const bool clustered_eligible =
                clustering && !is_hit && m.category >= 0 &&
                // NB: do NOT gate on cluster_key>=0 here. cluster_key is the old nearest-grace id
                // (-1 when no grace is near, e.g. underground items whose grace lookup misses), and
                // tile clustering groups by map-space tile, not grace proximity — gating on it would
                // wrongly exclude grace-less items (the Siofra "won't cluster" bug).
                // Graces are major waypoints — vanilla shows each individually, so never pile them.
                m.category != (int)goblin::generated::Category::WorldGraces &&
                // Only tile-cluster on LIVE-projected map-space. A marker still on the baked overworld
                // affine (e.g. an underground area the map VM hasn't resolved) has wrong map-space and
                // would scatter across tiles — draw it individually until its live projection lands.
                // (live_state is from last frame's project_marker; converges after one frame.)
                (m.live_state == 1 || !goblin::config::liveProjection) &&
                (dist_adaptive || goblin::ui::category_clustered(m.category));

            float gU, gV;
            project_marker(m, gU, gV);
            proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
            ImVec2 sp(p.x, p.y);
            // DEBUG (cluster_debug_markers): why a marker does/doesn't cluster. Dot colour = projection
            // state (green live / red baked-fallback / grey untried); text = its map-space tile (tu,tv)
            // and group. Same-tile+same-colour neighbours cluster; scattered tiles or red dots explain a
            // miss. Drawn for on-screen markers only.
            if (goblin::config::clusterDebugMarkers &&
                sp.x > -32 && sp.x < realW + 32 && sp.y > -32 && sp.y < realH + 32)
            {
                const ImU32 dc = m.live_state == 1 ? IM_COL32(60, 230, 90, 255)
                               : m.live_state == -1 ? IM_COL32(235, 70, 70, 255)
                                                    : IM_COL32(160, 160, 160, 255);
                fg->AddCircleFilled(ImVec2(sp.x + 6, sp.y - 6), 3.0f, dc);
                fg->AddCircle(ImVec2(sp.x + 6, sp.y - 6), 3.0f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
                const int tu = (int)(gU / goblin::worldmap::kTileSize);
                const int tv = (int)(gV / goblin::worldmap::kTileSize);
                char tb[48];
                std::snprintf(tb, sizeof(tb), "%d,%d g%d", tu, tv, m.group);
                fg->AddText(ImVec2(sp.x + 10, sp.y - 12), IM_COL32(255, 255, 255, 230), tb);
            }
            // Locate request (a clicked search result): consider this marker as a pan candidate (even
            // if off-screen — same page, so panning brings it into view). Best = uncollected-first,
            // then nearest to the view centre; finalised after the loop.
            if (s_locate_nameid && m.name_id == s_locate_nameid)
            {
                bool cleared_only = false;
                const bool uncollected = !marker_done(m, cleared_only);
                const float dd = (gU - locCenU) * (gU - locCenU) + (gV - locCenV) * (gV - locCenV);
                const bool better = !loc_have ||
                                    (uncollected && !loc_best_uncollected) ||
                                    (uncollected == loc_best_uncollected && dd < loc_best_d);
                if (better)
                {
                    loc_have = true;
                    loc_best_uncollected = uncollected;
                    loc_best_d = dd;
                    loc_best = ImVec2(gU, gV);
                }
            }
            const bool offscreen =
                (sp.x < -32 || sp.y < -32 || sp.x > realW + 32 || sp.y > realH + 32);
            if (offscreen)
            {
                if (!clustered_eligible)
                    continue; // off-screen + not a pile member → no gates, no draw
                // Clustered pile member: keep it only if its map-space cell can be on screen
                // (viewport rect + one-tile margin). A member further off than that lands in a cell
                // the viewport never touches, so its pile is off-screen too — skip the gates. Members
                // within the margin stay, so a cell straddling the edge keeps every member.
                if (gU < vMinU || gU > vMaxU || gV < vMinV || gV > vMaxV)
                    continue;
            }

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
            // acquired (m.fragment_flag = GetMapFlagFromTile, read live). Exception (vanilla
            // parity): an already-DISCOVERED grace shows regardless of fragment ownership.
            if (goblin::config::requireMapFragments && m.fragment_flag &&
                !goblin::ui::read_event_flag(static_cast<uint32_t>(m.fragment_flag)) &&
                !is_discovered_grace(m))
                continue; // map fragment not acquired yet (and not an already-discovered grace)

            // Clustered-eligible markers are deferred (uncull'd) to the pile pass;
            // everything else (already on-screen here) draws now.
            if (clustered_eligible)
            {
                clustered.push_back({sp, &m, gU, gV});
                ++n_deferred;
            }
            else
            {
                draw_marker(fg, m, sp, icons, iconHalf);
                // Search highlight: ring the matching markers so they stand out on the page.
                if (is_hit)
                    fg->AddCircle(sp, iconHalf * 1.7f, IM_COL32(255, 226, 40, 255), 0, 2.5f);
                hover_test(hover, mouse, sp, iconHalf, [&] { return marker_label(m); });
                ++n_drawn;
            }
        }
    }
    // Finalise the locate: pan onto the chosen candidate (best uncollected / nearest). If NO candidate
    // was found, the marker is on a page that isn't open — KEEP the request pending so it fires the
    // moment that page opens (the auto-switch marshal is bringing it). HOLD the pan while a page switch
    // is still settling: the engine snaps the new page's view, which would clobber a too-early pan.
    if (s_locate_nameid && loc_have && !goblin::worldmap_probe::page_switch_busy())
    {
        s_locate_pos = loc_best;
        s_locate_have = true;
        s_locate_nameid = 0; // satisfied
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

    // [ICONTIER] audit: throttled summary of how this pass's markers resolved across the tier chain.
    tier_census_flush();
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
    // Item 13 (dx-bugs-backlog): the minimap used to hardcode half=6.0f, completely ignoring the
    // same scale settings the worldmap honors. Clamped so an extreme scale setting can't make the
    // small fixed-radius HUD unreadable or blow past its own icons.
    constexpr float kMinimapIconHalfBase = 6.0f; // matches the old fixed size at scale=1
    float half = kMinimapIconHalfBase * cfg::overlayMasterScale * cfg::overlayIconScale;
    half = half < 3.0f ? 3.0f : (half > 10.0f ? 10.0f : half);

    // Item 13: lightweight screen-space clustering. The minimap's projection is a simple local,
    // player-centred Euclidean one (not the worldmap's pan/zoom u,v system), so this buckets by
    // rounding each marker's screen offset to a fixed-size cell instead of reusing the worldmap's
    // draw_clusters — that's coupled to hover/tooltips/distance-adaptive zoom logic that doesn't
    // apply to a small fixed-radius HUD widget.
    struct MiniHit { const Marker *m; ImVec2 pos; };
    std::unordered_map<uint64_t, std::vector<MiniHit>> cells;
    constexpr float kCellPx = 14.0f;
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
            // Vanilla parity: an already-discovered grace bypasses this gate.
            if (cfg::requireMapFragments && m.fragment_flag &&
                !goblin::ui::read_event_flag(static_cast<uint32_t>(m.fragment_flag)) &&
                !is_discovered_grace(m))
                continue;
            const auto cx = static_cast<int32_t>(std::floor(dx / kCellPx));
            const auto cy = static_cast<int32_t>(std::floor(dy / kCellPx));
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(m.group)) << 40) ^
                                  (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 20) ^
                                  static_cast<uint64_t>(static_cast<uint32_t>(cy));
            cells[key].push_back({&m, ImVec2(ctr.x + dx, ctr.y + dy)});
        }
    }
    // Item 14: same yellow ring the worldmap draws around an active item-search "locate" target
    // (search_hit is a TU-local helper, already visible here — no plumbing needed).
    for (auto &cell : cells)
    {
        const std::vector<MiniHit> &hits = cell.second;
        bool any_search_hit = false;
        ImVec2 avg(0.f, 0.f);
        for (const MiniHit &h : hits)
        {
            avg.x += h.pos.x;
            avg.y += h.pos.y;
            if (search_hit(*h.m))
                any_search_hit = true;
        }
        avg.x /= static_cast<float>(hits.size());
        avg.y /= static_cast<float>(hits.size());
        if (hits.size() == 1)
        {
            draw_marker(fg, *hits[0].m, hits[0].pos, icons, half);
        }
        else
        {
            fg->AddCircleFilled(avg, half + 1.5f, IM_COL32(40, 42, 60, 220));
            fg->AddCircle(avg, half + 1.5f, IM_COL32(230, 220, 180, 200), 0, 1.5f);
            char countBuf[8];
            std::snprintf(countBuf, sizeof(countBuf), "%d", static_cast<int>(hits.size()));
            const ImVec2 ts = ImGui::CalcTextSize(countBuf);
            fg->AddText(ImVec2(avg.x - ts.x * 0.5f, avg.y - ts.y * 0.5f),
                       IM_COL32(255, 255, 255, 255), countBuf);
        }
        if (any_search_hit)
            fg->AddCircle(avg, half * 1.7f, IM_COL32(255, 226, 40, 255), 0, 2.0f);
    }
    fg->PopClipRect();

    // Player marker at centre + a north tick (no heading yet → north-up only).
    fg->AddCircleFilled(ctr, 4.0f, IM_COL32(255, 225, 70, 255));
    fg->AddCircle(ctr, 4.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    fg->AddText(ImVec2(ctr.x - 4.f, ctr.y - R - 16.f), IM_COL32(230, 220, 180, 220), "N");
}
} // namespace goblin::worldmap
