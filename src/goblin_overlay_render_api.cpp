// Slice C wrapper implementations — see goblin_overlay_render_api.hpp for the design note. Every
// function here is a mechanical one-line forward to the real host symbol; no logic lives here.

#include "goblin_overlay_render_api.hpp"

#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"  // err_features_enabled
#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_debug_events.hpp"
#include "goblin_markers.hpp"
#include "goblin_kindling.hpp"
#include "goblin_worldmap_probe.hpp"
#include "goblin_logic.hpp"
#include "goblin_messages.hpp"
#include "goblin_map_data.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_param_edit.hpp"          // param_set_field bridge (World Editor)
#include "goblin_world_bundle.hpp"        // world-bundle save/apply bridge (World Editor)
#include "goblin_geom_move.hpp"           // live placement move bridge (World Editor)
#include "goblin_warp.hpp"                // warp_to_grace bridge (vmap click-to-warp)
#include "worldmap/loot_disk.hpp"
#include "worldmap/map_entry_layer.hpp"   // rebuild_markers (refresh_markers RPC)
#include "worldmap/name_fmg_en.hpp"
#include "input/input_shared.hpp"
#include "input/input_cursor.hpp"
#include "goblin_overlay.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Virtual world map open-flag (panel_virtual_map.cpp) — forward-declared to avoid pulling the
// D3D12-coupled panel_internal.hpp here; both live in the render module (single-DLL build).
namespace goblin::overlay::panel { bool &virtual_map_open(); void virtual_map_request_fit();
                                   void virtual_map_set_group(int g); int virtual_map_group();
                                   void virtual_map_request_tile(const char *needle, float, float, float, float);
                                   void virtual_map_clear_tiles();
                                   std::string virtual_map_load_lod(int dim, int lod, int cap);
                                   std::string virtual_map_load_resident();
                                   std::string virtual_map_tile_recon();
                                   int virtual_map_locate(int32_t name_id, int group);
                                   std::string virtual_map_offmap_probe();
                                   void virtual_map_item_search(const char *query);
                                   void virtual_map_force_spiderfy(bool on);
                                   void virtual_map_set_relief(bool on);
                                   int dump_markers_csv(const char *path);
                                   void virtual_map_set_view(float, float, float);
                                   void virtual_map_set_flip(bool, bool); }
namespace goblin::overlay { void request_f1_tab(int idx); }  // goblin_overlay_render.cpp

namespace goblin::overlay_api
{
#define GOBLIN_CFG_DEF_PTR(name) \
    decltype(&goblin::config::name) cfg_##name##_ptr() { return &goblin::config::name; }
    GOBLIN_CFG_BOOL_LIST(GOBLIN_CFG_DEF_PTR)
    GOBLIN_CFG_FLOAT_LIST(GOBLIN_CFG_DEF_PTR)
    GOBLIN_CFG_U8_LIST(GOBLIN_CFG_DEF_PTR)
    GOBLIN_CFG_U16_LIST(GOBLIN_CFG_DEF_PTR)
#undef GOBLIN_CFG_DEF_PTR

    bool *cfg_showCategory_ptr() { return goblin::config::showCategory; }
    std::string &cfg_questProgress_ref() { return goblin::config::questProgress; }
    std::string &cfg_regionToggles_ref() { return goblin::config::regionToggles; }
    std::string &cfg_uiExclusionRects_ref() { return goblin::config::uiExclusionRects; }

    bool section_visible(int s) { return goblin::ui::section_visible(s); }
    void set_section_visible(int s, bool v) { goblin::ui::set_section_visible(s, v); }
    bool category_visible(int c) { return goblin::ui::category_visible(c); }
    void set_category_visible(int c, bool v) { goblin::ui::set_category_visible(c, v); }
    uint32_t visibility_generation() { return goblin::ui::visibility_generation(); }

    // Teleport to an ABSOLUTE world XZ (unified marker frame) — same logic as the warp_xyz RPC:
    // keep the current tile, offset the player's local pos by (target − currentRaw). Intra-region only
    // (a far cross-map target may land in unstreamed void). Returns false if not in-world / write failed.
    bool warp_to_world_xz(float wx, float wz)
    {
        float lx, ly, lz;
        if (!goblin::get_player_world_pos(lx, ly, lz)) return false;
        int area = 0; float cwx = 0.f, cwz = 0.f;
        if (!goblin::get_player_raw_pos(area, cwx, cwz)) return false;
        return goblin::write_player_local_pos(lx + (wx - cwx), ly, lz + (wz - cwz), /*set_y=*/false);
    }
    const char *category_label(int c) { return goblin::ui::category_label(c); }
    const char *section_label(int idx) { return goblin::ui::section_label(idx); }
    int category_section(int c) { return goblin::ui::category_section(c); }
    int category_count() { return goblin::ui::category_count(); }
    int section_count() { return goblin::ui::section_count(); }
    int category_total(int c) { return goblin::ui::category_total(c); }
    int category_remaining(int c) { return goblin::ui::category_remaining(c); }
    bool category_clustered(int c) { return goblin::ui::category_clustered(c); }
    void set_category_clustered(int c, bool v) { goblin::ui::set_category_clustered(c, v); }
    bool clustering_enabled() { return goblin::ui::clustering_enabled(); }
    void set_clustering_enabled(bool v) { goblin::ui::set_clustering_enabled(v); }
    int global_threshold() { return goblin::ui::global_threshold(); }
    void set_global_threshold(int v) { goblin::ui::set_global_threshold(v); }
    bool icons_enabled() { return goblin::ui::icons_enabled(); }
    void set_icons_enabled(bool v) { goblin::ui::set_icons_enabled(v); }
    void request_native_landmark_reapply() { goblin::request_native_landmark_reapply(); }
    bool err_hide_bosses() { return goblin::ui::err_hide_bosses(); }
    void set_err_hide_bosses(bool v) { goblin::ui::set_err_hide_bosses(v); }
    void note_menu_visible() { goblin::ui::note_menu_visible(); }
    bool quest_unfinishable(size_t i) { return goblin::ui::quest_unfinishable(i); }
    bool read_event_flag(uint32_t id) { return goblin::ui::read_event_flag(id); }
    void request_cluster_replan() { goblin::ui::request_cluster_replan(); }
    void rebuild_markers() { goblin::worldmap::rebuild_markers(); }
    void virtual_map_set_open(bool open) { goblin::overlay::panel::virtual_map_open() = open; }
    bool virtual_map_is_open() { return goblin::overlay::panel::virtual_map_open(); }
    void f1_request_tab(int idx) { goblin::overlay::request_f1_tab(idx); }
    void virtual_map_fit() { goblin::overlay::panel::virtual_map_request_fit(); }
    void virtual_map_set_group(int g) { goblin::overlay::panel::virtual_map_set_group(g); }
    void virtual_map_request_tile(const char *needle, float wx0, float wz0, float wx1, float wz1)
    { goblin::overlay::panel::virtual_map_request_tile(needle, wx0, wz0, wx1, wz1); }
    void virtual_map_clear_tiles() { goblin::overlay::panel::virtual_map_clear_tiles(); }
    std::string virtual_map_load_lod(int dim, int lod, int cap)
    { return goblin::overlay::panel::virtual_map_load_lod(dim, lod, cap); }
    std::string virtual_map_load_resident() { return goblin::overlay::panel::virtual_map_load_resident(); }
    std::string virtual_map_tile_recon() { return goblin::overlay::panel::virtual_map_tile_recon(); }
    int virtual_map_locate(int32_t name_id, int group) { return goblin::overlay::panel::virtual_map_locate(name_id, group); }
    std::string virtual_map_offmap_probe() { return goblin::overlay::panel::virtual_map_offmap_probe(); }
    void virtual_map_item_search(const char *query) { goblin::overlay::panel::virtual_map_item_search(query); }
    void virtual_map_force_spiderfy(bool on) { goblin::overlay::panel::virtual_map_force_spiderfy(on); }
    void virtual_map_set_relief(bool on) { goblin::overlay::panel::virtual_map_set_relief(on); }
    int virtual_map_dump_markers(const char *path) { return goblin::overlay::panel::dump_markers_csv(path); }
    bool warp_to_grace(int32_t graceId, int32_t offset) { return goblin::warp::to_grace(graceId, offset); }
    void virtual_map_set_view(float camX, float camZ, float zoom)
    { goblin::overlay::panel::virtual_map_set_view(camX, camZ, zoom); }
    void virtual_map_set_flip(bool flipX, bool flipZ)
    { goblin::overlay::panel::virtual_map_set_flip(flipX, flipZ); }
    int virtual_map_get_group() { return goblin::overlay::panel::virtual_map_group(); }
    bool param_set_field(const char *param, uint64_t row, const char *field, double value)
    {
        if (!param || !field) return false;
        std::wstring wp(param, param + std::strlen(param));  // ASCII param names
        if (!goblin::paramedit::field_is_known(wp.c_str(), field)) return false;
        return goblin::paramedit::param_set_field_by_name(wp.c_str(), row, field, value);
    }
    bool param_get_field(const char *param, uint64_t row, const char *field, double *out)
    {
        if (!param || !field || !out) return false;
        std::wstring wp(param, param + std::strlen(param));  // ASCII param names
        if (!goblin::paramedit::field_is_known(wp.c_str(), field)) return false;
        auto v = goblin::paramedit::param_get_field_by_name(wp.c_str(), row, field);
        if (!v) return false;
        *out = *v;
        return true;
    }
    bool param_clone(const char *param, uint64_t srcRow, int32_t newId)
    {
        if (!param) return false;
        std::wstring wp(param, param + std::strlen(param));  // ASCII param names
        return goblin::paramedit::param_clone_row(wp.c_str(), srcRow, newId);
    }
    int we_scan() { return goblin::world_editor::scan(); }
    size_t we_asset_count() { return goblin::world_editor::asset_count(); }
    size_t we_goods_count() { return goblin::world_editor::goods_count(); }
    size_t we_copy_assets(goblin::world_editor::WEAsset *out, size_t max)
    {
        return goblin::world_editor::copy_assets(out, max);
    }
    size_t we_copy_goods(goblin::world_editor::WEGoods *out, size_t max)
    {
        return goblin::world_editor::copy_goods(out, max);
    }
    void we_bundle_record_set(const char *param, uint64_t row, const char *field, double value)
    {
        if (param && field) goblin::world_bundle::record_set(param, row, field, value);
    }
    void we_bundle_record_clone(const char *param, uint64_t src, int32_t newId)
    {
        if (param) goblin::world_bundle::record_clone(param, src, newId);
    }
    size_t we_bundle_count() { return goblin::world_bundle::op_count(); }
    std::string we_bundle_status() { return goblin::world_bundle::status_line(); }
    bool we_bundle_save() { return goblin::world_bundle::save_default(); }
    int we_bundle_apply() { return goblin::world_bundle::apply_default(); }
    void we_bundle_clear() { goblin::world_bundle::clear(); }
    bool we_move_near(float dx, float dy, float dz, float out_before[3], float out_now[3],
                      float *out_dist)
    {
        float dist = -1.0f;
        auto r = goblin::geom_move::move_near(dx, dy, dz, dist);
        if (out_dist) *out_dist = dist;
        if (out_before) { out_before[0] = r.before[0]; out_before[1] = r.before[1]; out_before[2] = r.before[2]; }
        if (out_now) { out_now[0] = r.moved[0]; out_now[1] = r.moved[1]; out_now[2] = r.moved[2]; }
        return r.ok;
    }
    bool we_move_aeg(int aegRow, float dx, float dy, float dz, float out_before[3], float out_now[3])
    {
        auto r = goblin::geom_move::move_aeg((uint32_t)aegRow, dx, dy, dz);
        if (out_before) { out_before[0] = r.before[0]; out_before[1] = r.before[1]; out_before[2] = r.before[2]; }
        if (out_now) { out_now[0] = r.moved[0]; out_now[1] = r.moved[1]; out_now[2] = r.moved[2]; }
        return r.ok;
    }
    bool we_move_restore() { return goblin::geom_move::restore_held().ok; }
    void request_save() { goblin::ui::request_save(); }
    void reset_quest_progress() { goblin::ui::reset_quest_progress(); }
    void reset_to_defaults() { goblin::ui::reset_to_defaults(); }
    void set_category_census(int idx, int total, int looted) { goblin::ui::set_category_census(idx, total, looted); }

    bool get_live_view(goblin::worldmap_probe::LiveView &out) { return goblin::worldmap_probe::get_live_view(out); }
    bool set_view_center(float mU, float mV, float minZoom) { return goblin::worldmap_probe::set_view_center(mU, mV, minZoom); }
    void set_locate_target(float u, float v) { goblin::worldmap_probe::set_locate_target(u, v); }
    void clear_locate_target() { goblin::worldmap_probe::clear_locate_target(); }
    bool locate_target_clamped() { return goblin::worldmap_probe::locate_target_clamped(); }
    bool err_features() { return goblin::err_features_enabled(); }
    bool page_switch_busy() { return goblin::worldmap_probe::page_switch_busy(); }
    void request_switch_to_page(int group) { goblin::worldmap_probe::request_switch_to_page(group); }
    const goblin::worldmap_probe::LocateDebug &last_locate_debug() { return goblin::worldmap_probe::last_locate_debug(); }

    bool world_map_open() { return goblin::world_map_open(); }
    size_t tpf_dds_count() { return goblin::tpf_dds_count(); }
    bool tpf_dds_at(size_t index, std::vector<uint8_t> &out) { return goblin::tpf_dds_at(index, out); }
    void force_load_file(const char *path) { goblin::force_load_file(path); }
    void force_graces() { goblin::force_graces(); }
    void force_create_icon(int iconId) { goblin::force_create_icon(iconId); }
    void force_create_last() { goblin::force_create_last(); }
    void bind_test(int mode, int groupId) { goblin::bind_test(mode, groupId); }
    std::string mask_to_combo_string(uint16_t mask) { return goblin::mask_to_combo_string(mask); }
    size_t harvested_count() { return goblin::harvested_count(); }
    std::vector<int> harvested_ids(size_t max) { return goblin::harvested_ids(max); }
    std::vector<goblin::GraceCandidate> grace_candidates() { return goblin::grace_candidates(); }

    size_t heightfield_snapshot(std::vector<goblin::heightfield::Cell> &out) { return goblin::heightfield::snapshot(out); }
    float heightfield_cell_step() { return goblin::heightfield::snapshot_step(); }
    void heightfield_request_sample(float extent, int res) { goblin::heightfield::request_sample(extent, res); }
    bool heightfield_sampling() { return goblin::heightfield::sampling(); }
    size_t far_relief_snapshot(std::vector<goblin::heightfield::Cell> &out) { return goblin::worldmap::far_relief_snapshot(out); }
    float far_relief_step() { return goblin::worldmap::far_relief_step(); }
    void far_relief_build(int group, int cellSize) { goblin::worldmap::build_far_relief(group, cellSize); }
    int far_relief_built_group() { return goblin::worldmap::far_relief_built_group(); }
    void set_grace_from_candidate(size_t index) { goblin::set_grace_from_candidate(index); }
    const std::vector<goblin::LiveGrace> &live_graces() { return goblin::live_graces(); }
    bool marker_world_pos(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz,
                          int &out_area, float &world_x, float &world_z, bool conv_underground)
    {
        return goblin::marker_world_pos(areaNo, gx, gz, px, pz, out_area, world_x, world_z, conv_underground);
    }
    int marker_fragment_flag(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz)
    {
        return goblin::marker_fragment_flag(areaNo, gx, gz, px, pz);
    }
    int marker_cluster_key(uint8_t area, uint8_t gridX, uint8_t gridZ, float posX, float posZ, int *out_pname)
    {
        return goblin::marker_cluster_key(area, gridX, gridZ, posX, posZ, out_pname);
    }
    int map_fragment_flag(int area, int gx, int gz) { return goblin::map_fragment_flag(area, gx, gz); }
    bool map_icon_rect_by_name(const char *name, int &x, int &y, int &w, int &h, void *&sheet) { return goblin::map_icon_rect_by_name(name, x, y, w, h, sheet); }
    size_t map_icon_layout_count() { return goblin::map_icon_layout_count(); }
    bool get_player_world_pos(float &x, float &y, float &z) { return goblin::get_player_world_pos(x, y, z); }
    bool get_player_map_pos(int &out_area, float &world_x, float &world_z, int *out_gx, int *out_gz, int *out_group)
    {
        return goblin::get_player_map_pos(out_area, world_x, world_z, out_gx, out_gz, out_group);
    }
    bool get_player_facing_yaw(float &yaw_radians) { return goblin::get_player_facing_yaw(yaw_radians); }
    int get_enemy_bar_labels(goblin::EnemyBarLabel *buf, int max) { return goblin::get_enemy_bar_labels(buf, max); }
    std::string lookup_text_utf8(int32_t id) { return goblin::lookup_text_utf8(id); }
    std::string lookup_name_en_disk_utf8(int32_t encoded_id) { return goblin::lookup_name_en_disk_utf8(encoded_id); }
    bool quest_step_done(const goblin::generated::NpcQuest &q, size_t s) { return goblin::quest_step_done(q, s); }
    uint32_t resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag) { return goblin::resolve_loot_flag(lotId, lotType, baked_flag); }
    int32_t resolve_loot_item_textid(uint32_t lotId, uint8_t lotType, int32_t baked_textid) { return goblin::resolve_loot_item_textid(lotId, lotType, baked_textid); }
    bool lot_row_in_table(uint32_t lot, uint8_t lotType, uint32_t *flagOut, int32_t *keyOut) { return goblin::lot_row_in_table(lot, lotType, flagOut, keyOut); }
    int lot_item_count(uint32_t lotId, uint8_t lotType) { return goblin::lot_item_count(lotId, lotType); }
    int lot_slot_item_keys(uint32_t lotId, uint8_t lotType, int32_t out[8]) { return goblin::lot_slot_item_keys(lotId, lotType, out); }
    void diag_loot_flags(uint32_t lotId, uint8_t lotType, uint32_t baked, int category, uint32_t nameId) { goblin::diag_loot_flags(lotId, lotType, baked, category, nameId); }
    int classify_item_live(int goodsId) { return goblin::classify_item_live(goodsId); }
    uint32_t npc_loot_lot(uint32_t npcParamId, uint8_t *lotTypeOut) { return goblin::npc_loot_lot(npcParamId, lotTypeOut); }
    int npc_item_lot_enemy(int npcId) { return goblin::npc_item_lot_enemy(npcId); }
    bool npc_team_and_name(uint32_t npcParamId, uint8_t *teamOut, int32_t *nameOut) { return goblin::npc_team_and_name(npcParamId, teamOut, nameOut); }
    bool aeg_is_gather(int assetId) { return goblin::aeg_is_gather(assetId); }
    int aeg_pickup_lot(int assetId) { return goblin::aeg_pickup_lot(assetId); }
    bool goods_is_map(int goodsId) { return goblin::goods_is_map(goodsId); }
    int item_marker_category(int goodsId) { return goblin::item_marker_category(goodsId); }
    int item_real_icon_id(int goodsId) { return goblin::item_real_icon_id(goodsId); }
    void gpu_want_symbol(const char *imgName) { goblin::gpu_want_symbol(imgName); }
    void gpu_want_item(int iconId) { goblin::gpu_want_item(iconId); }

    const char *markers_category_name(goblin::generated::Category c) { return goblin::markers::category_name(c); }
    bool markers_set_event_flag(uint32_t flag_id, uint8_t value) { return goblin::markers::set_event_flag(flag_id, value); }
    bool kindling_is_row_collected(uint64_t row_id) { return goblin::kindling::is_row_collected(row_id); }
    uint64_t kindling_region_row_id(const char *region_name) { return goblin::kindling::region_row_id(region_name); }

    bool is_original_row_collected(uint64_t original_row_id) { return goblin::collected::is_original_row_collected(original_row_id); }
    void register_runtime_entries(std::vector<goblin::collected::RuntimeEntry> entries) { goblin::collected::register_runtime_entries(std::move(entries)); }

    void debug_events_arm_capture(const char *npc_name) { goblin::debug_events::arm_capture(npc_name); }
    bool debug_events_capture_armed() { return goblin::debug_events::capture_armed(); }
    size_t debug_events_capture_count() { return goblin::debug_events::capture_count(); }
    int debug_events_finalize_capture(bool (*reader)(uint32_t)) { return goblin::debug_events::finalize_capture(reader); }

    int get_cursor_pos_real(void *point) { return goblin::input::get_cursor_pos_real(reinterpret_cast<LPPOINT>(point)); }
    bool input_menu_open() { return goblin::input::menu_open(); }

    std::filesystem::path disk_loot_dir() { return goblin::worldmap::disk_loot_dir(); }
    goblin::worldmap::DiskLootState disk_loot_state() { return goblin::worldmap::disk_loot_state(); }

    bool native_item_icon(int iconId, void *&tex, float &u0, float &v0, float &u1, float &v1)
    {
        return goblin::overlay::native_item_icon(iconId, tex, u0, v0, u1, v1);
    }
    bool native_map_point_icon(int iconId, void *&tex, float &u0, float &v0, float &u1, float &v1)
    {
        return goblin::overlay::native_map_point_icon(iconId, tex, u0, v0, u1, v1);
    }
    bool native_map_point_icon_by_name(const char *name, void *&tex, float &u0, float &v0,
                                       float &u1, float &v1)
    {
        return goblin::overlay::native_map_point_icon_by_name(name, tex, u0, v0, u1, v1);
    }
    bool map_point_glyph_uv(const char *name, int iconId, void *&tex, float &u0, float &v0,
                            float &u1, float &v1, int *outW, int *outH)
    {
        return goblin::overlay::map_point_glyph_uv(name, iconId, tex, u0, v0, u1, v1, outW, outH);
    }
}
