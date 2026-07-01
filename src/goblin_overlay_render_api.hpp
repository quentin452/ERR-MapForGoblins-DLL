#pragma once

// Slice C of docs/plans/overlay_hot_reload_playwright_plan.md: consolidated wrapper API the
// render-side code (src/goblin_overlay_render.cpp + src/worldmap/*.cpp) calls to reach host-only
// functionality, instead of calling goblin::config::*/goblin::ui::*/etc. directly. Existing host
// headers (goblin_config.hpp, goblin_inject.hpp, ...) are NOT touched — this file is the entire
// cross-DLL export surface, kept in one place so the eventual LoadLibrary boundary has a single,
// auditable list instead of dllexport annotations scattered across ~15 shared headers.
//
// Every declaration here is implemented in goblin_overlay_render_api.cpp by forwarding to the real
// (host-side) symbol. Types that cross the boundary by value/reference (GraceCandidate, LiveGrace,
// RuntimeEntry, SigHealth, LiveView, NpcQuest, DiskLootState, ...) are all declared in headers both
// sides already include (goblin_inject.hpp, goblin_collected.hpp, goblin_worldmap_probe.hpp,
// re_signatures.hpp, goblin_map_data.hpp, worldmap/loot_disk.hpp) — no export needed for the types
// themselves, only for the functions that operate on them.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "goblin_dll_export.hpp"      // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)
#include "goblin_inject.hpp"          // GraceCandidate, LiveGrace
#include "goblin_collected.hpp"       // RuntimeEntry
#include "goblin_worldmap_probe.hpp"  // LiveView, LocateDebug
#include "goblin_map_data.hpp"        // generated::Category
#include "goblin_quest_steps.hpp"     // generated::NpcQuest
#include "worldmap/loot_disk.hpp"     // DiskLootState

namespace goblin::overlay_api
{
    // ── goblin::config::* (59 of 64 globals — the other 5 are `inline constexpr`, already free) ──
    // Scalars are exposed as pointer-getters (works uniformly for read AND for the ImGui widgets
    // that bind `&goblin::config::X` directly) via one shared macro shape.
#define GOBLIN_CFG_BOOL_LIST(X) \
    X(requireMapFragments) X(bakedOnly) X(collectedGraying) X(hideCollected) X(stackIdenticalItems) \
    X(clusterDebugRadius) X(clusterDebugMarkers) X(showRegionLabels) X(nativeItemIcons) X(diagLootFlags) \
    X(diagLootPos) X(debugLogging) X(anonymousLoot) X(dropMerchantPhantoms) X(redifyBossIcons) \
    X(graceOverlay) X(graceGpuSprite) X(suppressNativeBosses) X(enableMarkerDump) X(debugEventFlags) \
    X(debugItemGrants) X(debugFlagCapture) X(debugWorldmapProbe) X(liveProjection) X(dumpIconTextures) \
    X(iconLegibility) X(altitudeCue) X(viewDelayZoom) X(debugClusterAnchors) X(debugRegionVolumes) \
    X(showMinimap) X(minimapAnchorRight) X(minimapAnchorBottom) X(questAllowFlagWrite) X(questGreyOnDeath) \
    X(clusterDistanceAdaptive)
#define GOBLIN_CFG_FLOAT_LIST(X) \
    X(altitudeDeadzone) X(graceIconScale) X(graceOffsetX) X(graceOffsetY) X(iconMinHalfPx) \
    X(mapSymbolScale) X(minimapOffsetX) X(minimapOffsetY) X(minimapOpacity) X(minimapSize) \
    X(minimapZoom) X(overlayClusterScale) X(overlayIconScale) X(overlayMasterScale) X(viewDelayFrames)
#define GOBLIN_CFG_U8_LIST(X) X(clusterFarRadius) X(clusterNearRadius) X(clusterNearThreshold) X(virtualKeyboardLayout)
#define GOBLIN_CFG_U16_LIST(X) X(overlayToggleGamepad)

#define GOBLIN_CFG_DECL_BOOL(name) GOBLIN_RENDER_API bool *cfg_##name##_ptr();
#define GOBLIN_CFG_DECL_FLOAT(name) GOBLIN_RENDER_API float *cfg_##name##_ptr();
#define GOBLIN_CFG_DECL_U8(name) GOBLIN_RENDER_API uint8_t *cfg_##name##_ptr();
#define GOBLIN_CFG_DECL_U16(name) GOBLIN_RENDER_API uint16_t *cfg_##name##_ptr();
    GOBLIN_CFG_BOOL_LIST(GOBLIN_CFG_DECL_BOOL)
    GOBLIN_CFG_FLOAT_LIST(GOBLIN_CFG_DECL_FLOAT)
    GOBLIN_CFG_U8_LIST(GOBLIN_CFG_DECL_U8)
    GOBLIN_CFG_U16_LIST(GOBLIN_CFG_DECL_U16)
#undef GOBLIN_CFG_DECL_BOOL
#undef GOBLIN_CFG_DECL_FLOAT
#undef GOBLIN_CFG_DECL_U8
#undef GOBLIN_CFG_DECL_U16

    GOBLIN_RENDER_API bool *cfg_showCategory_ptr();          // array base (bool showCategory[]), index at the call site
    GOBLIN_RENDER_API std::string &cfg_questProgress_ref();  // mutated in place (bit twiddling on the packed string)
    GOBLIN_RENDER_API std::string &cfg_regionToggles_ref();  // assigned wholesale (region-toggle bitset serialization)

    // ── goblin::ui::* (host-defined in src/goblin_section_visibility.cpp + goblin_inject.cpp) ──
    GOBLIN_RENDER_API bool section_visible(int s);
    GOBLIN_RENDER_API void set_section_visible(int s, bool v);
    GOBLIN_RENDER_API bool category_visible(int c);
    GOBLIN_RENDER_API void set_category_visible(int c, bool v);
    GOBLIN_RENDER_API const char *category_label(int c);
    GOBLIN_RENDER_API const char *section_label(int idx);
    GOBLIN_RENDER_API int category_section(int c);
    GOBLIN_RENDER_API int category_count();
    GOBLIN_RENDER_API int section_count();
    GOBLIN_RENDER_API int category_total(int c);
    GOBLIN_RENDER_API int category_remaining(int c);
    GOBLIN_RENDER_API bool category_clustered(int c);
    GOBLIN_RENDER_API void set_category_clustered(int c, bool v);
    GOBLIN_RENDER_API bool clustering_enabled();
    GOBLIN_RENDER_API void set_clustering_enabled(bool v);
    GOBLIN_RENDER_API int global_threshold();
    GOBLIN_RENDER_API void set_global_threshold(int v);
    GOBLIN_RENDER_API bool icons_enabled();
    GOBLIN_RENDER_API void set_icons_enabled(bool v);
    GOBLIN_RENDER_API bool err_hide_bosses();
    GOBLIN_RENDER_API void set_err_hide_bosses(bool v);
    GOBLIN_RENDER_API void note_menu_visible();
    GOBLIN_RENDER_API bool quest_unfinishable(size_t i);
    GOBLIN_RENDER_API bool read_event_flag(uint32_t id);
    GOBLIN_RENDER_API void request_cluster_replan();
    GOBLIN_RENDER_API void request_save();
    GOBLIN_RENDER_API void reset_quest_progress();
    GOBLIN_RENDER_API void reset_to_defaults();
    GOBLIN_RENDER_API void set_category_census(int idx, int total, int looted);

    // ── goblin::worldmap_probe::* (host, src/goblin_worldmap_probe.cpp) ──
    GOBLIN_RENDER_API bool get_live_view(goblin::worldmap_probe::LiveView &out);
    GOBLIN_RENDER_API bool set_view_center(float mU, float mV, float minZoom = 0.f);
    GOBLIN_RENDER_API void set_locate_target(float u, float v);
    GOBLIN_RENDER_API void clear_locate_target();
    GOBLIN_RENDER_API bool page_switch_busy();
    GOBLIN_RENDER_API void request_switch_to_page(int group);
    GOBLIN_RENDER_API const goblin::worldmap_probe::LocateDebug &last_locate_debug();

    // ── bare goblin::* (host, goblin_inject.cpp + its PR-split files) ──
    GOBLIN_RENDER_API bool world_map_open();
    GOBLIN_RENDER_API size_t tpf_dds_count();
    GOBLIN_RENDER_API bool tpf_dds_at(size_t index, std::vector<uint8_t> &out);
    GOBLIN_RENDER_API void force_load_file(const char *path);
    GOBLIN_RENDER_API void force_graces();
    GOBLIN_RENDER_API void force_create_icon(int iconId);
    GOBLIN_RENDER_API void force_create_last();
    GOBLIN_RENDER_API void bind_test(int mode, int groupId);
    GOBLIN_RENDER_API std::string mask_to_combo_string(uint16_t mask);
    GOBLIN_RENDER_API size_t harvested_count();
    GOBLIN_RENDER_API std::vector<int> harvested_ids(size_t max);
    GOBLIN_RENDER_API std::vector<goblin::GraceCandidate> grace_candidates();
    GOBLIN_RENDER_API void set_grace_from_candidate(size_t index);
    GOBLIN_RENDER_API const std::vector<goblin::LiveGrace> &live_graces();
    GOBLIN_RENDER_API bool marker_world_pos(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz,
                          int &out_area, float &world_x, float &world_z,
                          bool conv_underground = false);
    GOBLIN_RENDER_API int marker_fragment_flag(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz);
    // marker_group_from is `inline` in goblin_inject.hpp — compiles into both DLLs free, no wrapper.
    GOBLIN_RENDER_API int marker_cluster_key(uint8_t area, uint8_t gridX, uint8_t gridZ, float posX, float posZ,
                           int *out_pname = nullptr);
    GOBLIN_RENDER_API int map_fragment_flag(int area, int gx, int gz);
    GOBLIN_RENDER_API bool map_icon_rect_by_name(const char *name, int &x, int &y, int &w, int &h, void *&sheet);
    GOBLIN_RENDER_API size_t map_icon_layout_count();
    GOBLIN_RENDER_API bool get_player_world_pos(float &x, float &y, float &z);
    GOBLIN_RENDER_API bool get_player_map_pos(int &out_area, float &world_x, float &world_z,
                            int *out_gx = nullptr, int *out_gz = nullptr, int *out_group = nullptr);
    GOBLIN_RENDER_API std::string lookup_text_utf8(int32_t id);
    GOBLIN_RENDER_API std::string lookup_name_en_disk_utf8(int32_t encoded_id);
    GOBLIN_RENDER_API bool quest_step_done(const goblin::generated::NpcQuest &q, size_t s);
    GOBLIN_RENDER_API uint32_t resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag);
    GOBLIN_RENDER_API int32_t resolve_loot_item_textid(uint32_t lotId, uint8_t lotType, int32_t baked_textid);
    GOBLIN_RENDER_API bool lot_row_in_table(uint32_t lot, uint8_t lotType, uint32_t *flagOut, int32_t *keyOut);
    GOBLIN_RENDER_API int lot_item_count(uint32_t lotId, uint8_t lotType);
    GOBLIN_RENDER_API void diag_loot_flags(uint32_t lotId, uint8_t lotType, uint32_t baked, int category, uint32_t nameId);
    GOBLIN_RENDER_API int classify_item_live(int goodsId);
    GOBLIN_RENDER_API uint32_t npc_loot_lot(uint32_t npcParamId, uint8_t *lotTypeOut);
    GOBLIN_RENDER_API int npc_item_lot_enemy(int npcId);
    GOBLIN_RENDER_API bool npc_team_and_name(uint32_t npcParamId, uint8_t *teamOut, int32_t *nameOut);
    GOBLIN_RENDER_API bool aeg_is_gather(int assetId);
    GOBLIN_RENDER_API int aeg_pickup_lot(int assetId);
    GOBLIN_RENDER_API bool goods_is_map(int goodsId);
    GOBLIN_RENDER_API int item_marker_category(int goodsId);
    GOBLIN_RENDER_API int item_real_icon_id(int goodsId);
    GOBLIN_RENDER_API void gpu_want_symbol(const char *imgName);
    GOBLIN_RENDER_API void gpu_want_item(int iconId);

    // ── goblin::markers::* / goblin::kindling::* (host) ──
    GOBLIN_RENDER_API const char *markers_category_name(goblin::generated::Category c);
    GOBLIN_RENDER_API bool markers_set_event_flag(uint32_t flag_id, uint8_t value);
    GOBLIN_RENDER_API bool kindling_is_row_collected(uint64_t row_id);
    GOBLIN_RENDER_API uint64_t kindling_region_row_id(const char *region_name);

    // ── goblin::collected::* (host, src/goblin_collected.cpp) ──
    GOBLIN_RENDER_API bool is_original_row_collected(uint64_t original_row_id);
    GOBLIN_RENDER_API void register_runtime_entries(std::vector<goblin::collected::RuntimeEntry> entries);

    // ── goblin::debug_events::* (host, src/goblin_debug_events.cpp) ──
    GOBLIN_RENDER_API void debug_events_arm_capture(const char *npc_name);
    GOBLIN_RENDER_API bool debug_events_capture_armed();
    GOBLIN_RENDER_API size_t debug_events_capture_count();
    GOBLIN_RENDER_API int debug_events_finalize_capture(bool (*reader)(uint32_t));

    // ── goblin::input::* subset used by the render layer (host) ──
    GOBLIN_RENDER_API int get_cursor_pos_real(void *point /* LPPOINT, void* to avoid a windows.h include here */);
    GOBLIN_RENDER_API bool input_menu_open();

    // ── goblin::worldmap::disk_loot_* (host despite the `worldmap::` name — loot_disk.cpp is in
    //    GOBLIN_HOST_SOURCES, not the render group) ──
    GOBLIN_RENDER_API std::filesystem::path disk_loot_dir();
    GOBLIN_RENDER_API goblin::worldmap::DiskLootState disk_loot_state();

    // ── goblin::overlay::native_*/map_point_glyph_uv (host — owns the SheetTex/MapSymSrv/DiskSheet
    //    D3D12 caches, same class of coupling as the grace/icon-SRV wrappers above). Already public
    //    API (declared in goblin_overlay.hpp) within the single-binary build; still routed through
    //    this wrapper layer for a consistent, single cross-DLL surface once Slice C's real
    //    LoadLibrary split lands. ──
    GOBLIN_RENDER_API bool native_item_icon(int iconId, void *&tex, float &u0, float &v0, float &u1, float &v1);
    GOBLIN_RENDER_API bool native_map_point_icon(int iconId, void *&tex, float &u0, float &v0, float &u1, float &v1);
    GOBLIN_RENDER_API bool native_map_point_icon_by_name(const char *name, void *&tex, float &u0, float &v0,
                                       float &u1, float &v1);
    GOBLIN_RENDER_API bool map_point_glyph_uv(const char *name, int iconId, void *&tex, float &u0, float &v0,
                            float &u1, float &v1, int *outW = nullptr, int *outH = nullptr);
}
