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

#include <cstdint>
#include <vector>

namespace goblin::worldmap
{
// Provenance of a marker — where its existence + position actually came from, for the
// [COVERAGE] no-bake scoreboard (how much still depends on the static bake vs the live
// mod files / game memory). Baked = the static goblin_map_data bake (MAP_ENTRIES);
// DiskMSB = parsed from the ACTIVE mod's real MSB files (the disk loot/collectible/enemy/
// emevd passes); Live = read from live game params (field bosses via WorldMapPointParam,
// graces via BonfireWarpParam). Default Baked.
enum class Source : uint8_t { Baked, DiskMSB, Live };

// One absorbed member of an item-stack (see Marker.stacked). Holds just enough to recompute the
// member's collected state at render (mirrors marker_done's loot branch) so the stack's "xN" can
// deplete as nodes are gathered, and the stack grays only when EVERY member is collected.
struct StackedMember
{
    uint64_t row_id = 0;       // geom/kindling collected check
    int collected_flag = 0;    // event flag: this member's item picked up
    int count = 1;             // this member's own item count (lot_item_count)
};

// A single marker in UNIFIED world space, pre-classified to a map group.
struct Marker
{
    float worldX = 0, worldZ = 0;
    // group = isDLC*2 | isUG : 0 base-overworld, 1 base-underground, 2 DLC-overworld,
    // 3 DLC-underground. The renderer draws only the OPEN group's markers.
    int group = 0;
    int srcArea = 0;            // original areaNo (diagnostics / converter choice)
    int category = -1;          // static_cast<int>(Category); drives the cluster opt-in
    int cluster_key = -1;       // nearest-grace group id (native by-location clustering); -1 = exact
    int loc_pname = -1;         // group's region PlaceName id (for the pile name label)
    int name_id = -1;           // this marker's own name textId (FMG lookup for the tooltip)
    unsigned int color = 0xEB82E65Au; // packed ImU32 ABGR — circle-fallback colour
    // Atlas cell key (goblin::overlay_icons ICON_CELLS, e.g. "show_graces"). When the
    // atlas is loaded and the key resolves, the marker draws as that icon; otherwise it
    // falls back to a coloured circle.
    const char *icon_key = nullptr;
    // Map-fragment discovery flag for this marker's tile (goblin::map_fragment_flag).
    // 0 = no fragment gate. When config::requireMapFragments is on, the renderer hides
    // the marker until this event flag is set (player found the area's map fragment).
    int fragment_flag = 0;
    // ── Collected/cleared graying (collected_graying config) ──────────────────────
    // All 0 = not applicable (e.g. graces) → never grayed. Set only by MapEntryLayer.
    uint64_t row_id = 0;     // original MAP_ENTRIES row id → rune/ember/kindling collected check
    int cleared_flag = 0;    // clearedEventFlagId: boss/NPC cleared → dim + green checkmark
    int collected_flag = 0;  // textDisableFlagId1: loot picked up → dim
    // Graces: their per-grace discovery flag (textDisableFlagId1). When SET (player has
    // discovered/rested there), the game draws that grace natively (generated from
    // BonfireWarpParam), so the renderer DROPS this overlay marker to avoid a double
    // icon. Undiscovered (flag unset) → drawn as the helper marker. 0 = no gate.
    int discover_flag = 0;
    // Grace in a DUNGEON (legacy/minor dungeon or DLC dungeon, projected to the overworld — NOT an
    // open-world-surface or underground grace). When ERR is installed, the renderer draws these with
    // the ERR dungeon-style grace icon (MENU_MAP_ERR_GraceUnderground) instead of the vanilla bonfire.
    // Set by GraceLayer (after the aggregate init); default false keeps the positional init intact.
    bool dungeon = false;
    // Post-event "secondary story flag" (legacy SetSecondaryFlags + Ashen Capital). When
    // SET this marker is a post-event variant (burnt/changed area) and only appears once
    // the story flag fires — the renderer HIDES it while the flag is unset (inverse of
    // discover_flag). 0 = no gate. Set only by MapEntryLayer.
    int secondary_flag = 0;
    // Post-event "hide" story flag — the INVERSE of secondary_flag. When SET this marker
    // is a PRE-event variant (e.g. Leyndell Royal Capital, area 11) that is replaced once
    // the story flag fires (the Ashen Capital takes over), so the renderer HIDES it the
    // moment the flag is set. 0 = no gate. Set only by MapEntryLayer. Mirrors the legacy
    // refresh_royal_eviction (which only ever ran on native injected rows).
    int hide_when_flag = 0;
    // Lot-backed loot marker (its item comes from an ItemLotParam row). Drives
    // anonymous_loot spoiler-free mode: when that config is on, this marker draws as a
    // gray "?" with a generic label instead of its real icon/name. Set only by
    // MapEntryLayer (false for pieces/kindling/bosses/graces — never lot-backed loot).
    bool lot_backed = false;
    // Raw param coords (area-local) for the engine's live projection (config
    // live_projection → worldmap_probe::project). raw_area < 0 = not set (skip live).
    int raw_area = -1, raw_gx = 0, raw_gz = 0;
    float raw_px = 0.0f, raw_pz = 0.0f;
    // Cached live map-space UV (filled lazily at render when the map is open).
    // live_state: 0 = untried, 1 = projected ok, -1 = engine didn't place it.
    mutable float live_u = 0.0f, live_v = 0.0f;
    mutable signed char live_state = 0;
    // Cached live PAGE (0 overworld, 1 base-UG, 10 DLC; -1 untried). Lets the group be
    // derived without the baked fold: group = (page==10?2:0)|((area==12||40-43)?1:0).
    mutable int live_page = -1;
    // ── Provenance (for the [COVERAGE] no-bake scoreboard) ────────────────────────
    // Where this marker came from. Set by the build sites (push_marker param); default
    // Baked so any unstamped path is conservatively counted as still-baked.
    Source source = Source::Baked;
    // True when this marker's CATEGORY was resolved via the LIVE classify_item_live
    // fallback (the baked item→category table missed it — an unbaked / new mod item).
    // Orthogonal to `source`: a DiskMSB marker may or may not be live-classified.
    bool live_classified = false;
    // Source ItemLotParam lot (loot markers) — for the [BAKED-RESIDUAL] bulk diag + offline
    // recovery cross-ref. 0 for non-lot markers (pieces / world features / graces). Set by
    // MapEntryLayer push_marker (after the aggregate init; default keeps other ctors intact).
    uint32_t lotId = 0;
    uint8_t  lotType = 0;
    // This loot's OWN inventory iconId (MapEntry.iconId); 0 = none. Lets the renderer draw the real
    // per-item icon instead of the category representative. Set by MapEntryLayer push_marker after
    // the aggregate init (default 0 keeps other ctors intact).
    int item_icon_id = 0;
    // DX item 7: block-local altitude (MSB pos[1]); ≈ world Y on the overworld. Set by push_marker after
    // the aggregate init (default 0 keeps other ctors intact). Drives the above/below-player altitude badge.
    float worldY = 0.0f;
    // Off-page altitude reference: the block-local Y of the nearest grace in this marker's SAME area
    // (precomputed at build from live_graces). When the marker is on a DIFFERENT page than the player,
    // the altitude badge compares worldY against this grace instead of the player (whose Y is in another
    // frame). has_ref_grace=false → no grace in the area → no off-page badge. Same-area only: worldY is
    // block-local, so the reference must share the frame. See docs/plans/offpage_altitude_via_grace_plan.md.
    float ref_grace_y = 0.0f;
    bool has_ref_grace = false;
    // Item count this marker represents on its OWN: the lot's deterministic quantity
    // (goblin::lot_item_count — a slot's num when one slot is live, else 1; see that fn). 1 for
    // non-lot markers. This stays the marker's own count even when it is a stack representative — the
    // STACK total/remaining is computed from `stacked` at render, so the stack toggle needs no rebuild.
    int count = 1;
    // Item stacking (computed ONCE at build by annotate_item_stacks, NON-destructively — the toggle
    // is a pure render decision, like clustering, so it's instant and needs no bucket rebuild):
    //   • Representative of a co-located identical-item group: `stacked` holds the per-member collected
    //     state of EVERY member (incl. itself); `stack_member` is false. When the stack toggle is ON it
    //     draws once with the group's summed/remaining " xN"; when OFF it draws as its own marker.
    //   • Non-representative member of a group: `stack_member` is true, `stacked` empty. HIDDEN when the
    //     toggle is ON (the representative stands in); drawn individually when OFF.
    //   • Marker in no group: both empty/false → always drawn with its own count.
    std::vector<StackedMember> stacked;
    bool stack_member = false;
    // Quest-NPC tooltip extras (QuestNpcLayer only; static hand-table strings, so plain
    // pointers — no allocation). marker_label() appends "quest — step\nzone" under the name.
    // MUST stay LAST: several Markers are built with positional aggregate init, so a new
    // field can only be appended (not inserted mid-struct).
    const char *tip_quest = nullptr; // NpcQuest::quest_title
    const char *tip_step = nullptr;  // active QuestStep::title (the current "palier")
    const char *tip_zone = nullptr;  // active QuestStep::zone (coarse location)
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
