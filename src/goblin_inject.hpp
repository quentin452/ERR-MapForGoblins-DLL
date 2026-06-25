#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace goblin
{
    void inject_map_entries();

    // Seed per-category visibility / cluster opt-in / threshold + master gate from
    // config. Runs in BOTH modes (inject_map_entries also seeds, but is skipped when
    // native_map_injection is off, so the overlay needs this independent path).
    void seed_runtime_gates();

    // Nearest-grace cluster key for a marker (source area + raw grid/pos), matching
    // the native by-location clustering. -1 = no anchor (draw exact). out_pname (opt)
    // = the group's region PlaceName id for the pile label.
    int marker_cluster_key(uint8_t area, uint8_t gridX, uint8_t gridZ, float posX,
                           float posZ, int *out_pname = nullptr);

    // Player + grace position in the RAW per-area frame (NO projection) — for overlap-free
    // distance-adaptive clustering (gate on same raw area; underground sub-maps stay
    // distinct via gridX*256). false during a load / on probe failure / bad key.
    bool get_player_raw_pos(int &out_area, float &wx, float &wz);
    bool grace_anchor_raw(int key, int &out_area, float &wx, float &wz);

    // Region PlaceName id for a marker via the game's MapNameOverride volumes (point-in-
    // volume containment in the marker's MSB-local frame). 0 = no volume here. The reliable
    // location-name source for tooltips + cluster labels (cities, region borders).
    int region_name_pname(uint8_t area, uint8_t gx, uint8_t gz, float posX, float posZ);

    // Project a grace anchor (GRACE_ANCHORS index = a marker's cluster_key) to unified
    // world coords, so a cluster pile can be drawn AT its grace. False on a bad key.
    bool grace_anchor_world(int key, int &out_area, float &wx, float &wz);
    // Map sub-page (tabId) of a grace anchor by its GRACE_ANCHORS index (cluster_key).
    // Used for underground distance-adaptive (same-sub-page = detail). -1 = bad key.
    int grace_anchor_tab(int key);
    // The player's current sub-page (tabId) from the reliable MapId tile. -1 = overworld
    // / unresolved. Underground distance-adaptive uses this (not the garbage float).
    int player_map_tab();
    // Raw tile (gridX/gridZ) of a grace anchor by index — for underground TILE-distance
    // distance-adaptive (reliable tile, garbage float). false = bad key.
    bool grace_anchor_tile(int key, int &out_gx, int &out_gz);

    // Cluster label census: (PlaceName textId, label) for each cluster the inject
    // built. The label is "<Region> (<count>)" (region via cluster_region_label) or
    // just "<count>" if the region is unknown. setup_messages injects it as the
    // cluster's PlaceName string (shown by the cluster-label toggle).
    const std::vector<std::pair<int, std::string>> &cluster_label_census();

    // Local player's world position (WorldChrMan chain, AOB-resolved + cached).
    // Returns false if not yet resolvable (early load) or the chain faulted.
    // For proximity clustering (v2). Coordinate space = global/world (verify).
    bool get_player_world_pos(float &x, float &y, float &z);

    // Player position in MARKER space via the CONFIRMED Target-A chain (see
    // docs/re_findings_playerpos.md): player MapId singleton (gridX/gridZ) +
    // CSWorldGeomMan block-local (+0x70/+0x74). Both statics AOB-anchored (patch-
    // resilient). out_area = page (60/61 overworld, 12 underground, …); world_x/z =
    // gridXZ*256 + local, directly comparable to a marker's gridXNo*256 + posX
    // WITHIN THE SAME area. Caveat: nested-region tile may be off ~±2 (negligible
    // for coarse proximity). Returns false until resolvable / on fault.
    // out_gx/out_gz (optional) = the player's reliable map TILE (from MapId, valid
    // even underground where the +0x70 local float is leaf-block-local garbage) —
    // used for tile-distance proximity in non-256 frames (area 12 / DLC underground).
    // out_group (optional) = the player's overlay marker GROUP (marker_group_from of the
    // raw + projected area): 0 base-OW, 1 base-UG, 2 DLC-OW, 3 DLC-UG → which map page the
    // player is on (the minimap / distance-adaptive use it to pick the player's markers).
    bool get_player_map_pos(int &out_area, float &world_x, float &world_z,
                            int *out_gx = nullptr, int *out_gz = nullptr,
                            int *out_group = nullptr);

    // Unified overworld marker-space coord for an arbitrary baked marker (projects
    // legacy dungeons to area-60 via LEGACY_CONV, then world = grid*256 + local).
    // Used by the overlay-rendered-markers prototype to place graces etc.
    // conv_underground=true also unifies base underground (area 12) into overworld
    // map-space (the underground map shares it; overlay draws it on the UG layer).
    bool marker_world_pos(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz,
                          int &out_area, float &world_x, float &world_z,
                          bool conv_underground = false);

    // Overlay marker GROUP (which map page it belongs to) from the ORIGINAL areaNo + the
    // PROJECTED area (`out_area` from marker_world_pos): 0 base-overworld, 1 base-underground,
    // 2 DLC-overworld, 3 DLC-underground. DLC by the final page (61 / 40-43); UNDERGROUND by
    // the original areaNo (12 / 40-43) — area 12 projects to overworld map-space (pg 60) so it
    // must gate by its SOURCE layer, not the final page. ONE place so the grace + map-entry
    // layers and the player-pos debug gate identically. The renderer draws only open_grp.
    inline int marker_group_from(uint8_t orig_area, int projected_area)
    {
        int pg = projected_area & 63;
        // The DLC map has NO separate underground sub-page — DLC areas 40-43 share the
        // ONE Land-of-Shadow page with area 61 (verified in-game: Gravesite Plain and the
        // "DLC underground" areas render on the same page, no depth toggle). So they group
        // as plain DLC, NOT DLC-underground. Only base-game area 12 is a true underground
        // layer. (group 3 = DLC-underground is therefore never produced → the DLC eyeball
        // projection it gated is dead.)
        bool isug = (orig_area == 12);
        bool isdlc = (pg == 61) || (orig_area >= 40 && orig_area <= 43);
        return (isdlc ? 2 : 0) | (isug ? 1 : 0);
    }

    // Map-fragment discovery flag for a marker, on the tile the native injection gates
    // on (projects overworld dungeons to area 60 first, like legacy GetMapFragment).
    // 0 = no fragment. Use this (not the raw-tile map_fragment_flag) for the overlay
    // require_map_fragments gate — the raw tile misses dungeon-interior cells → leaks.
    int marker_fragment_flag(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz);

    // (marker_fogged removed — the WorldMapPieceParam "fog" was the map-fragment region reveal,
    // redundant with require_map_fragments. A per-tile walk-fog gate was prototyped and dropped
    // (non-issue in normal play). RE kept in docs/re/windows_worldmap_tile_fog_re_findings.md.)

    // A Site of Grace read LIVE from BonfireWarpParam — no baked data, no MASSEDIT.
    struct LiveGrace
    {
        uint8_t areaNo, gridXNo, gridZNo;
        float posX, posZ;
        int textId;        // textId1 = place-name (resolve via MsgRepository)
        uint64_t rowId;
        int discoverFlag;  // eventflagId = per-grace discovery flag (set when reached)
        bool underground;  // iconId == 44 → ERR cave/underground grace icon (else normal bonfire)
        int subCat;        // bonfireSubCategoryId → region PlaceName (when valid) + tab via subcat param
    };
    // Capture all grace rows from the LIVE BonfireWarpParam (the engine's own grace table).
    void capture_live_graces();
    // The captured grace list (empty until capture_live_graces() ran). Used by the
    // ImGui overlay path instead of the baked MAP_ENTRIES graces.
    const std::vector<LiveGrace> &live_graces();

    // An authoritative named-location anchor (a Site of Grace). wx/wz = raw world coords
    // (gridX*256 + local). placename_id = region PlaceName id rendered as the cluster label
    // (0 = no name → count-only). Built LIVE from live_graces() + BonfireWarpSubCategoryParam
    // (replaces the old baked GRACE_ANCHORS / grace_position_index). gridX/gridZ/posX/posZ kept
    // so the overlay can project an anchor through marker_world_pos (pile placed AT its grace).
    struct GraceAnchor
    {
        uint8_t  area;
        float    wx, wz;
        int32_t  placename_id;
        int32_t  tab_id;     // map sub-page (underground area 12 splits 12000/12001/12002)
        uint8_t  gridX, gridZ;
        float    posX, posZ;
    };
    // Lazily built on first call (needs the PlaceName FMG patch, which runs after capture).
    const std::vector<GraceAnchor> &grace_anchors();

    // Resolve a lot-backed loot marker's LIVE pickup flag (ItemLotParam getItemFlagId),
    // falling back to baked_flag. Lets the overlay detect collected loot without the
    // native injection's refresh_loot_from_itemlot. lotType: 1=_map, 2=_enemy, 0=none.
    uint32_t resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag);

    // Resolve a lot-backed marker's IDENTITY (offset-encoded name/icon key) from the LIVE
    // ItemLotParam row (slot-1 item id @+0x00 + category @+0x20), so the marker shows the
    // item ERR/randomizer actually placed instead of the baked vanilla one. Returns the
    // baked key on any miss. Feeds the marker label (FMG) + item_icon_id().
    int32_t resolve_loot_item_textid(uint32_t lotId, uint8_t lotType, int32_t baked_textid);

    // One-shot RE diagnostic (config diag_loot_flags): for a sample of loot lots per
    // category, log every candidate "obtained" flag (lot-wide @0x80, the 8 per-slot
    // getItemFlagId0N @0x60, baked textDisableFlagId1) AND whether each currently reads
    // SET — so on a 100% save the column that is SET reveals the real collected flag.
    void diag_loot_flags(uint32_t lotId, uint8_t lotType, uint32_t baked, int category,
                         uint32_t nameId);

    // Region gating for the overlay (mirrors the game's native areaNo+tab display).
    // grace_tab_id: the map sub-page (tabId) of the nearest GRACE_ANCHOR in this
    // SOURCE area (12000/12001 underground, 6800-6999 DLC, …); -1 if none. Lets the
    // overlay tell DLC graces (area 22/25/28 project onto base 60/61 but carry a DLC
    // tab) from base ones. raw_wx/wz = SOURCE-area world (gridXNo*256+posX, pre-conv).
    int grace_tab_id(uint8_t src_area, float raw_wx, float raw_wz);
    // True when the PLAYER is currently in the DLC (Land of Shadow / DLC underground),
    // from the live MapId tile → its tabId (6800-6999) or native DLC area 40-43.
    bool player_in_dlc();


    // True once WorldMapPointParam is loaded — the robust init wait polls this
    // instead of sleeping a fixed load_delay (slow PCs can take >5s). SEH-guarded.
    bool world_map_param_ready();

    // Data pointers of MFG-injected WorldMapPointParam rows in the expanded
    // table. Populated by inject_map_entries(); consumed by
    // sanitize_injected_textids() after the FMG bank is built.
    const std::vector<uint8_t *> &injected_row_ptrs();

    // Rewrites rows baked with a primary completion flag to its alternative
    // once the alternative flag turns on (quest fights with two mutually-
    // exclusive outcome flags, e.g. the Sellen/Jerren academy battle).
    // Called periodically from the refresh loop.
    void apply_flag_or_pairs();

    // Per-category uncollected census. Sweeps g_section_rows, buckets by category,
    // and caches "collectible total" + "remaining (uncollected)" per category for
    // the overlay to show next to each checkbox. Reads the game flag API, so it
    // runs on the watcher thread — and ONLY while the overlay panel is on-screen
    // (ui::note_menu_visible stamps a deadline), so the 9296-row sweep costs
    // nothing when the menu is closed. Throttled to 1s.
    int refresh_category_census();

    // Part 2 (Quest Browser): recompute, for each QUEST_BROWSER entry, whether
    // its questline is now UNFINISHABLE — its NpcQuest::fail_flag event flag is
    // set (NPC dead early / interconnection lost). Reads flags on the watcher
    // thread; the overlay reads the cached result via ui::quest_unfinishable().
    int refresh_quest_finishable();

    // Row ids used for both the TutorialParam rows AND the TutorialBody.fmg
    // entries holding each banner's STATIC text. Injected by
    // inject_tutorial_popup_rows() (param table) and goblin_messages
    // setup_messages() (FMG bank). All texts are static (no runtime FMG
    // rewrite — that approach crashed; each distinct message gets its own id).
    // Chosen just past the highest existing ERR codex id (9004250) — keeps
    // ids close to the original range to avoid any internal int32-cast or
    // size-bucket assumptions that hit far-out values.
    constexpr int TUTORIAL_FMG_ID_ON        = 9004251;  // "Map icons: ON"
    constexpr int TUTORIAL_FMG_ID_OFF       = 9004252;  // "Map icons: OFF"
    constexpr int TUTORIAL_FMG_ID_DUMP_OK   = 9004253;  // "Markers dumped"
    constexpr int TUTORIAL_FMG_ID_DUMP_FAIL = 9004254;  // "Marker dump failed - press again"
    constexpr int TUTORIAL_FMG_ID_COVERAGE_GAP = 9004255; // "unmapped item collected (coverage gap)"

    // (9004260..73 was the per-section toggle-banner block — removed when section
    // toggles stopped firing toasts; the overlay menu owns section visibility now.)

    // Per-category coverage-gap toasts. The item category is read from the
    // granted item id's high nibble (Armament/Armour/Talisman/Goods/Ash of War/
    // other). Base kept at 9004280 (clear of the freed 9004260..73 block).
    constexpr int TUTORIAL_FMG_ID_GAP_CAT_BASE = 9004280; // base .. base+COUNT-1
    constexpr int GAP_CAT_COUNT                = 6;
    inline constexpr const wchar_t *GAP_CAT_NAMES[GAP_CAT_COUNT] = {
        L"Armament", L"Armour", L"Talisman", L"Goods", L"Ash of War", L"item"};
    inline int gap_cat_toast_id(int cat) { return TUTORIAL_FMG_ID_GAP_CAT_BASE + cat; }

    // Inject the codex-toast TutorialParam rows for the F10/F9 banners. Rows
    // get menuType=0 (upper-left codex caption widget) with textId pointing
    // at TutorialBody.fmg entries injected by goblin_messages.
    // Returns true on success.
    bool inject_tutorial_popup_rows();

    // Fire an upper-left codex-style toast for one of the injected TutorialParam
    // rows (pass a TUTORIAL_FMG_ID_* id). Static text, no FMG rewrite — same
    // path as the F10 banner. Safe from any thread once init has run.
    void show_codex_toast(int tutorial_id);

    // Queue a codex toast (by TUTORIAL_FMG_ID_*) to be fired by the watcher
    // thread, spaced so multiple don't overwrite each other. Thread-safe; the
    // preferred entry point for runtime toasts (e.g. coverage-gap notices).
    void enqueue_toast(int tutorial_id);

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the overlay menu's master / per-section / per-category / cluster
    // intents, persists them, and shows the master toggle banner.
    void menu_auto_toggle_loop();

    // True if a section toggle currently keeps this row's param data hidden
    // (areaNo forced to 99). Other areaNo owners (fragment-eviction restore,
    // collected restore-all) must consult this before restoring a row to its
    // visible area, else they un-hide section-hidden rows. Thread-safe.
    bool is_section_hidden_ptr(const void *param_data);

    // True when the in-game 2D world map screen is open (CSMenuMan+0xCD == 7).
    // Resolves the CSMenuMan singleton once via AOB; returns false until warm or
    // if resolution fails. Safe from any thread (used by the overlay to show
    // itself only over the map).
    bool world_map_open();

    // ── Live world-map icon refresh (EXPERIMENTAL, config::liveRefreshWorldMap) ──
    // Dev probe (config dump_icon_textures): hook CSScaleformImageCreator::CreateImage to
    // log each worldmap icon image (sprite rect + backing GPU texture) → crack iconId↔image.
    void install_icon_texture_probe();

    // Background icon-harvest tick. Drives the proactive repo-tree walk
    // (harvest_repo_icons → map_icon_rect rects + grace sprites). SAFE to call from any
    // thread: read-only RPM on our own process, publishes under g_map_icon_mtx /
    // g_harvest_mtx. Self-throttled (re-walks only on resident-count change, rate-capped in
    // production). No-op unless an icon-consuming config is on. Called from the
    // worldmap_probe background poll instead of the game-thread find hook so Wine's per-RPM
    // cost never stalls the engine (see docs/rpm_walk_audit.md, [[linux-rpm-walk-danger]]).
    void background_harvest_tick();

    // Hook the WarpPinData builder to suppress native discovered-grace pins (config
    // grace_suppress_native). Phase A logs [WARPPIN] to confirm identification (RE e4b3f6a).
    void install_grace_suppression_hook();

    // Dev probe (dump_icon_textures): iterate EquipParam* live, log rows whose iconId matches
    // the inventory-captured MENU_FL_<iconId> sprites → proves item↔iconId↔sprite.
    void verify_equip_iconids();

    // Path A verify (findings §6): on a MAP-OPEN frame, re-read each registered image's
    // img+0x10 (lazily-bound Render::Texture) → GXTexture2D+0x40 = ID3D12Resource. Call from
    // the worldmap probe loop while the map is open; runs once per session after it resolves.
    void dump_icon_textures_live();

    // A resolved item-icon sprite: the engine's sheet ID3D12Resource + the sub-rect on it + the
    // sheet's GetDesc() info. `sheet` is engine-owned (do NOT Release). Resolved draw-free via the
    // find-by-name chain (sprite findings §1/§2). valid=false ⇒ miss / sheet not resident (caller
    // falls back to the baked PNG). Cached per iconId (valid only). Feeds the future DX12 atlas copy.
    struct ItemSprite
    {
        void *sheet = nullptr;                 // ID3D12Resource* of the loaded sheet
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // sub-rect on the sheet
        unsigned long long sheetW = 0;
        unsigned int sheetH = 0, format = 0;   // DXGI_FORMAT (from GetDesc)
        bool valid = false;
    };
    ItemSprite resolve_item_sprite(int iconId);

    // Look up an item icon HARVESTED live by the find-hook (resident menu icons; their sheet
    // resource + sub-rect + DXGI_FORMAT). Thread-safe (engine thread writes, render thread reads).
    // Returns false if that iconId hasn't been seen/loaded yet → caller falls back to the baked PNG.
    bool harvested_icon(int iconId, ItemSprite &out);

    // Resolve a marker/item key (offset-encoded item id, == the worldmap PlaceName textId for
    // item markers) to its real inventory iconId via the baked ITEM_ICONS table. Returns -1 if
    // the key isn't an item (boss/grace/NPC names miss) → the marker keeps its category atlas icon.
    int item_icon_id(int32_t key);

    // Marker/item key → the MFG Category that item would get as a normal marker
    // (static_cast<int>), or -1 if the item isn't in the baked ITEM_ICONS
    // classifier. Lets the disk-MSB loot path bucket a live-resolved lot item
    // without the bake. Same lookup as item_icon_id() (ITEM_ICONS carries both).
    int item_marker_category(int32_t key);

    // Placed AEG asset's collectible item-lot, resolved LIVE from
    // AssetEnvironmentGeometryParam[aegRow].pickUpItemLotParamId (an ItemLotParam_map
    // id), or 0 if the asset is not a pickup. aegRow = AEG{A}_{B} → A*1000+B. Feeds the
    // disk collectible source (loot_collectibles) — no bake, no manual model→item map.
    uint32_t aeg_pickup_lot(uint32_t aegRow);

    // True if the AEG asset is a one-time GATHER node (AssetEnvironmentGeometryParam
    // [aegRow].isEnableRepick, byte 0x3c bit 6). This is the bake's gather filter
    // (isEnableRepick ⇔ isHiddenOnRepick for every pickup asset — verified), so the
    // collectible pass can place every gather node generically instead of the _8xx
    // model-range heuristic — separating gather nodes from one-shot clutter (pots/jars/
    // corpses, isEnableRepick=false). No bake. aegRow = AEG{A}_{B} → A*1000+B.
    bool aeg_is_gather(uint32_t aegRow);

    // Placed enemy's drop item-lot, resolved LIVE from NpcParam[npcParamId]: prefers
    // itemLotId_map (sets *lotTypeOut=1) over itemLotId_enemy (sets *lotTypeOut=2), 0 if
    // none. npcParamId = the MSB Enemy part's NPCParamID. Feeds the disk enemy-drop source
    // (loot_enemy_drops) — no bake. See memory msbe-enemy-loot-offsets.
    uint32_t npc_loot_lot(uint32_t npcParamId, uint8_t *lotTypeOut);

    // Diag-only: the RAW itemLotId_enemy (s32 @ +0x30) for an MSB enemy's NpcParam, ignoring the
    // map-lot preference npc_loot_lot applies. Used by the [ENEMY-MARKERS] de-bake triage to tell
    // "enemy parsed but its enemy-lot wasn't covered (map-preferred / filtered)" from "not parsed".
    int32_t npc_item_lot_enemy(uint32_t npcParamId);
    int32_t npc_item_lot_map(uint32_t npcParamId);  // raw itemLotId_map (s32 @ +0x34), same diag use

    // Live NpcParam teamType (u8 @ +0x133) + nameId (s32 @ +0xc) for an MSB enemy's
    // npcParamId. The no-bake Hostile NPC pass uses it: a named invader = teamType ∈ {24,27}
    // AND nameId > 0 (a real NpcName entry — the canonical signal separating invaders from
    // mobs that merely share the team). Returns false if the row is absent / params not ready.
    // Offsets pinned vs the paramdef layout + raw rows (tools/probe_npcparam_offsets.py:
    // itemLot 0x30/0x34 cross-check + 25/25 invaders confirmed).
    bool npc_team_and_name(uint32_t npcParamId, uint8_t *teamOut, int32_t *nameOut);

    // True iff an EquipParamGoods row is a region Map fragment (sortGroupId u8 @ +0x72 ∈
    // {190 base, 191 DLC}). The no-bake World-Maps pass routes map-good pickups to WorldMaps.
    bool goods_is_map(int32_t goods_id);

    // Probe one ItemLotParam row in a SPECIFIC table (lotType 1=_map, 2=_enemy; NO
    // fallback to the other), for the EMEVD sequence-sibling walk (loot_emevd_drops
    // mechanism C). Returns true iff the row EXISTS in that table (false = a GAP → stop the
    // walk). When it exists: *flagOut = the notability flag (resolve_loot_flag semantics —
    // 0 for none/repeatable), *keyOut = the slot-1 encoded item key (0 if empty/invalid).
    // The caller emits a sub-lot marker when the row exists with flag != 0 and a real item.
    bool lot_row_in_table(uint32_t lot, uint8_t lotType, uint32_t *flagOut, int32_t *keyOut);

    // Live category fallback when item_marker_category() (baked ITEM_ICONS) misses:
    // derives a GENERIC MFG Category from the LIVE item type (EquipParamGoods.goodsType
    // for goods, the lot category for equipment). Takes the offset-encoded item key
    // (encode_live_item). -1 if unknown. Makes the disk loot/collectible source work for
    // any mod's items, not just ones the ERR bake classified.
    int classify_item_live(int32_t key);

    // Count of icons harvested so far (dev/diagnostic — shown in the P2b test panel).
    size_t harvested_count();

    // DEV force-load TEST (RE windows_resident_icon_enumeration_re_findings §5b): stream a TPF/file
    // RESIDENT by FD4 path via the CSFile singleton — load(CSFile=*(er+0x3d5b0f8), wchar* path, 0, 0)
    // at er+0x1f5560. Returns the resource handle (0 on miss/null singleton). Lets us test whether a
    // non-resident icon sheet can be force-loaded on demand. Gated by config::dumpIconTextures; called
    // from the P2b panel button. utf8_path e.g. "menu:/00_Solo.tpf".
    void *force_load_file(const char *utf8_path);

    // DEV bind-flip TEST (RE findings §5e): confirm that flipping a loaded resource-GROUP entry's
    // +0x7c "needs-apply" flag triggers the binding (apply vmethod resource+0xc8) that populates the
    // repo with per-icon RECTS — i.e. whether a non-resident item-icon group can be forced resident +
    // bound on demand. Queues a one-shot action consumed on the next residency tick (engine thread,
    // via the FUN_140d724c0 hook). action: 1=dump groups (log apply-vmethod RVA + flags), 2=load
    // files(groupId) via the by-id loaders, 3=flip-bind all loaded groups, 4=load+flip(groupId).
    // Returns false if the manager isn't captured yet (open inventory/map once). Gated by dumpIconTextures.
    bool bind_test(int action, int groupId);

    // DEV force-bind a single item icon (RE §5g): replay CSScaleformImageCreator::CreateImage
    // (FUN_140d6bbc0) for the symbol "MENU_ItemIcon_<iconId>" on the engine thread — the GFx per-image
    // bind callback that find/creates the repo image. Lets us test forcing a non-browsed icon's bind
    // without driving the menu. Queue-based (needs a captured context — open the inventory once first).
    // Gated by dumpIconTextures. Returns false if no manager/context captured yet.
    bool force_create_icon(int iconId);

    // Control for force_create_icon: replay the most-recent live CreateImage symbol (a known-good
    // "img://…" import) to prove the replay mechanism works end-to-end. Gated by dumpIconTextures.
    bool force_create_last();

    // Manually re-run the grace force-CreateImage (F1 "Force graces now") — harvests the
    // MENU_MAP_*/SB_ERR_Grace_* gfx-movie sprites on demand, bypassing the auto cap/lock.
    bool force_graces();

    // Browser over distinct DDS captured from ER's decompresses (grabbed in the Oodle hook before ER
    // freed each transient buffer). count + fetch-by-index; the overlay uploads each into its own
    // texture to locate the icon sheet. No game GPU bind, no read-after-free, mod-agnostic.
    size_t tpf_dds_count();
    bool tpf_dds_at(size_t i, std::vector<uint8_t> &out);

    // Map-point icon LAYOUT (RAM-captured from the sblytbnd via the Oodle hook; force-load
    // "menu:/01_common.sblytbnd" to trigger it — it loads at boot before the hook). Resolves a
    // WORLD_MAP_POINT_PARAM.iconId (the MENU_MAP_<NN> SubTexture) to its rect on the SB_MapCursor
    // sheet (err=true → the SB_MapCursor_ERR sheet). Named lookup covers MENU_MAP_ERR_*/Church/etc.
    // Returns false until the layout is captured / if the id|name is absent. RE
    // windows_map_point_icon_layout_re_findings.md.
    size_t map_icon_layout_count();
    // x,y,w,h = sub-rect on the sheet; sheet = the backing ID3D12Resource (crop UV = rect/sheetDims).
    bool map_icon_rect(int iconId, int &x, int &y, int &w, int &h, void *&sheet);
    bool map_icon_rect_by_name(const char *name, int &x, int &y, int &w, int &h, void *&sheet);

    // First `max` harvested iconIds (dev — the P2b test panel draws ACTUAL harvested icons
    // instead of a hardcoded id list that may not match what the player browsed).
    std::vector<int> harvested_ids(size_t max);

    // The discovered/lit grace sprite (SB_ERR_Grace_Morning_Color), harvested by the find hook when
    // the open map draws a discovered grace. False until seen at least once. Lets the overlay draw
    // graces itself (discovered = this sprite, undiscovered = grey-tinted).
    bool harvested_grace(ItemSprite &out);

    // The ERR dungeon-style grace sprite (MENU_MAP_ERR_GraceUnderground). False until captured / if
    // ERR isn't installed. The overlay uses it for DUNGEON graces in place of the vanilla bonfire.
    bool harvested_grace_dungeon(ItemSprite &out);

    // All SB_ERR_Grace_* sprite frames seen on the sheet (dev debug — the F1 grace viewer draws each
    // so we can visually pick the correct grace rect). spr carries sheet/rect/dims/format.
    struct GraceCandidate { ItemSprite spr; std::string name; };
    std::vector<GraceCandidate> grace_candidates();

    // DEV (F1 grace-debug): set the active grace sprite from a captured candidate index (test which
    // name maps to the right grace). Locks it; the overlay should then rebuild its SRV. False on bad idx.
    bool set_grace_from_candidate(size_t idx);

    // Dev runtime-confirm (sprite findings §6): call the engine's draw-free icon
    // find-by-name FUN_140d63c30(repo, &out, L"MENU_ItemIcon_<id>") for a set of iconIds and
    // log whether each resolves to a CSTextureImage+rect WITHOUT the icon being drawn. Press
    // with the inventory OPEN (sheets resident), then CLOSED, to measure the residency limit.
    // Bound to F8, gated dump_icon_textures. [FIND2] log lines.
    void probe_icon_find_runtime();

    // ── Overlay control API ──────────────────────────────────────────────
    // Read/post the same runtime state the F-key hotkeys drive. Setters only
    // POST intents (atomics) — the watcher thread (menu_auto_toggle_loop) stays
    // the single owner of game-state mutation. Safe to call from the render
    // thread (the overlay).
    namespace ui
    {
        int section_count();
        const char *section_label(int idx);
        bool section_visible(int idx);
        void set_section_visible(int idx, bool visible);

        bool icons_enabled();              // master on/off (inverse of user-disabled)
        void set_icons_enabled(bool on);

        // The 63 fine-grained marker categories (live park-all). category_section
        // returns the section index the category belongs to (for grouping in UI).
        int category_count();
        const char *category_label(int idx);
        int category_section(int idx);
        bool category_visible(int idx);
        void set_category_visible(int idx, bool visible);
        // Per-category uncollected census (drives the "<remaining>/<total>" badge
        // next to each category). category_total = collectible rows in the
        // category; category_remaining = uncollected (total - looted), or -1 when
        // the category has no collectible rows (graces/NPCs/regions → no badge).
        // Cached by refresh_category_census() on the watcher thread. note_menu_visible
        // must be called each frame the panel is drawn to keep the census warm.
        int category_total(int idx);
        int category_remaining(int idx);
        // Publish a category's census from the overlay (total collectible + looted).
        void set_category_census(int idx, int total, int looted);
        void note_menu_visible();
        // ERR integration: hide our boss markers since ERR marks bosses natively.
        bool err_hide_bosses();
        void set_err_hide_bosses(bool hide);
        // Per-category cluster opt-in (true = folds into clusters). Editing takes
        // effect after Save + restart, since the cluster plan is built at inject.
        bool category_clustered(int idx);
        void set_category_clustered(int idx, bool clustered);
        // Per-category cluster threshold (effective: override or global default).
        // Persisted into cluster_threshold_overrides on Save; restart to apply.
        int  category_threshold(int idx);
        void set_category_threshold(int idx, int threshold);
        int  global_threshold();
        void set_global_threshold(int t);

        // Persist the current section/category visibility to the ini (the menu's
        // Save button). Posts a request; the watcher thread does the file I/O.
        void request_save();

        bool clustering_enabled();         // config flag (what a restart would use)
        void set_clustering_enabled(bool on);  // takes effect after Save + restart
        bool clusters_expanded();
        void set_clusters_expanded(bool expanded);
        // Hard (mixed-category piles) vs Soft (per-category). LIVE — re-plans the
        // clusters at runtime (applied on next map open), no restart.
        bool cluster_hard();
        void set_cluster_hard(bool on);
        // Re-plan request (overlay writes the distance-adaptive knobs/presets to
        // config::* directly, then asks for a live re-plan on next map open).
        void request_cluster_replan();
        // Quest-aware NPC gating (live; persisted on Save). Show a curated
        // questline NPC's marker only while its quest is active.
        bool quest_aware();
        void set_quest_aware(bool on);
        bool cluster_debug();
        void set_cluster_debug(bool on);
        // Danger zone. reset_quest_progress clears all quest-step checkmarks live
        // (persisted on next Save). reset_to_defaults posts a request; the watcher
        // re-seeds config from defaults + writes the ini (restart to fully apply).
        void reset_quest_progress();
        void reset_to_defaults();
        // Part 2: true if QUEST_BROWSER[i]'s questline is unfinishable (its
        // fail_flag is set). Cached by refresh_quest_finishable() on the watcher.
        bool quest_unfinishable(size_t i);
        // Live event-flag read (wraps the internal IsEventFlag resolver) — used
        // by the flag-capture tool's finalize re-check. bool(*)(uint32_t).
        bool read_event_flag(uint32_t id);
    }
};
