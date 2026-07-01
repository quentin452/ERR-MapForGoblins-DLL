// Slice C wrapper implementations — see goblin_overlay_render_api.hpp for the design note. Every
// function here is a mechanical one-line forward to the real host symbol; no logic lives here.

#include "goblin_overlay_render_api.hpp"

#include "goblin_config.hpp"
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
#include "worldmap/loot_disk.hpp"
#include "worldmap/name_fmg_en.hpp"
#include "input/input_shared.hpp"
#include "input/input_cursor.hpp"
#include "goblin_overlay.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

    bool section_visible(int s) { return goblin::ui::section_visible(s); }
    void set_section_visible(int s, bool v) { goblin::ui::set_section_visible(s, v); }
    bool category_visible(int c) { return goblin::ui::category_visible(c); }
    void set_category_visible(int c, bool v) { goblin::ui::set_category_visible(c, v); }
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
    bool err_hide_bosses() { return goblin::ui::err_hide_bosses(); }
    void set_err_hide_bosses(bool v) { goblin::ui::set_err_hide_bosses(v); }
    void note_menu_visible() { goblin::ui::note_menu_visible(); }
    bool quest_unfinishable(size_t i) { return goblin::ui::quest_unfinishable(i); }
    bool read_event_flag(uint32_t id) { return goblin::ui::read_event_flag(id); }
    void request_cluster_replan() { goblin::ui::request_cluster_replan(); }
    void request_save() { goblin::ui::request_save(); }
    void reset_quest_progress() { goblin::ui::reset_quest_progress(); }
    void reset_to_defaults() { goblin::ui::reset_to_defaults(); }
    void set_category_census(int idx, int total, int looted) { goblin::ui::set_category_census(idx, total, looted); }

    bool get_live_view(goblin::worldmap_probe::LiveView &out) { return goblin::worldmap_probe::get_live_view(out); }
    bool set_view_center(float mU, float mV, float minZoom) { return goblin::worldmap_probe::set_view_center(mU, mV, minZoom); }
    void set_locate_target(float u, float v) { goblin::worldmap_probe::set_locate_target(u, v); }
    void clear_locate_target() { goblin::worldmap_probe::clear_locate_target(); }
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
