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

    // Project a grace anchor (GRACE_ANCHORS index = a marker's cluster_key) to unified
    // world coords, so a cluster pile can be drawn AT its grace. False on a bad key.
    bool grace_anchor_world(int key, int &out_area, float &wx, float &wz);

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
    bool get_player_map_pos(int &out_area, float &world_x, float &world_z,
                            int *out_gx = nullptr, int *out_gz = nullptr);

    // Unified overworld marker-space coord for an arbitrary baked marker (projects
    // legacy dungeons to area-60 via LEGACY_CONV, then world = grid*256 + local).
    // Used by the overlay-rendered-markers prototype to place graces etc.
    // conv_underground=true also unifies base underground (area 12) into overworld
    // map-space (the underground map shares it; overlay draws it on the UG layer).
    bool marker_world_pos(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz,
                          int &out_area, float &world_x, float &world_z,
                          bool conv_underground = false);

    // Map-fragment discovery flag for a marker, on the tile the native injection gates
    // on (projects overworld dungeons to area 60 first, like legacy GetMapFragment).
    // 0 = no fragment. Use this (not the raw-tile map_fragment_flag) for the overlay
    // require_map_fragments gate — the raw tile misses dungeon-interior cells → leaks.
    int marker_fragment_flag(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz);

    // Fog-of-war reveal test (RE: docs/re/windows_fog_reveal_mask_re_findings.md). True
    // when the marker at MAP-SPACE (mx,my) on the given areaIdx layer (0=overworld,
    // 1=underground, 10=DLC) sits on a WorldMapPieceParam piece whose reveal event flag is
    // unset = still fogged. This is the engine's real fog state (replaces marker_fragment_
    // flag once the map-space↔openTravelArea transform is calibrated). Builds the piece
    // cache live on first call + logs [FOGCAL] calibration data.
    bool marker_fogged(int areaIdx, float mx, float my);

    // A Site of Grace read LIVE from WorldMapPointParam (iconId 370) — no baked data.
    struct LiveGrace
    {
        uint8_t areaNo, gridXNo, gridZNo;
        float posX, posZ;
        int textId;        // name (resolve via MsgRepository)
        uint64_t rowId;
        int discoverFlag;  // textDisableFlagId1 = per-grace discovery flag (set when rested)
    };
    // Capture all grace rows from the LIVE WorldMapPointParam. MUST run at init BEFORE
    // inject_map_entries() swaps the param backing (else it reads our injected rows).
    void capture_live_graces();
    // The captured grace list (empty until capture_live_graces() ran). Used by the
    // ImGui overlay path instead of the baked MAP_ENTRIES graces.
    const std::vector<LiveGrace> &live_graces();

    // Resolve a lot-backed loot marker's LIVE pickup flag (ItemLotParam getItemFlagId),
    // falling back to baked_flag. Lets the overlay detect collected loot without the
    // native injection's refresh_loot_from_itemlot. lotType: 1=_map, 2=_enemy, 0=none.
    uint32_t resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag);

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

    // True if this injected row is a Leyndell Ashen Capital (m35) marker, which
    // apply_map_logic gates on StoryErdtreeOnFire (shows only after the burn).
    bool is_ashen_capital_row(uint64_t row_id);

    // Rewrites rows baked with a primary completion flag to its alternative
    // once the alternative flag turns on (quest fights with two mutually-
    // exclusive outcome flags, e.g. the Sellen/Jerren academy battle).
    // Called periodically from the refresh loop.
    void apply_flag_or_pairs();

    // Live-loot (config::liveLootFlags): read each loot marker's source
    // ItemLotParam row from live memory and set textDisableFlagId1 to the
    // lot's current getItemFlagId, so markers hide on the actual light-point
    // pickup for the loaded regulation (Item/Enemy Randomizer compatible).
    // One-shot, called once after inject_map_entries().
    void refresh_loot_from_itemlot();

    // Toggle the WorldMapPointParam swap between vanilla and expanded states.
    // Used as an ERSC-hosting workaround: revert before host, re-apply after.
    void set_param_injection_active(bool active);
    bool is_param_injection_active();

    // ── Fragment-eviction (map-open perf) ──────────────────────────────
    // A row whose gating event flag (map-fragment / story flag) isn't set yet
    // costs nothing to display but the game STILL processes it on every map
    // open (the icon is merely hidden at draw time). Parking such a row off-page
    // (areaNo = 99) removes it from the per-page open cost entirely, the same
    // 1-byte trick collected/kindling use. apply_map_logic registers each
    // gate-flagged goblin row here (except those collected/kindling already own
    // areaNo for); refresh_fragment_eviction() — called from the refresh loop,
    // where event flags are reliably readable — parks undiscovered rows and
    // restores them (areaNo back to original) the moment their flag turns on.
    void register_fragment_gated_row(void *param_data, uint8_t original_area,
                                     uint32_t gate_flag);
    int refresh_fragment_eviction();

    // Thread 4: hide Leyndell Royal Capital (areaNo 11) markers once the Erdtree
    // burns (StoryErdtreeOnFire). Inverse of the ashen gate; park-only. Call from
    // the refresh loop AFTER refresh_fragment_eviction so the hide wins.
    int refresh_royal_eviction();
    int refresh_quest_npc_eviction();
    int refresh_cluster_depletion();

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
    // Resolve + queue the hook on the engine's placed-map-point (re)build
    // (FUN_140a82a80). Must run BEFORE modutils::enable_hooks() applies the queued
    // hooks. No-op unless config::liveRefreshWorldMap is set, so default builds are
    // unaffected. See docs/windows_re_live_refresh_capture.md.
    void install_live_refresh_hook();

    // Request a live icon refresh: the next time the engine runs its build the
    // detour replays it once more with the engine's own captured (this, ctx), so
    // an areaNo edit applied while the map is open re-renders without a reopen.
    // No-op if the hook isn't installed or the map isn't open. Thread-safe (sets an
    // atomic; the actual rebuild runs on the engine's thread).
    void refresh_world_map_icons();

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

        bool clustering_active();          // clusters built this session (live controls valid)
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
