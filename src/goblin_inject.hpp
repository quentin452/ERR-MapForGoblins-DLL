#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace goblin
{
    void inject_map_entries();

    // Cluster label census: (PlaceName textId, member count) for each cluster the
    // inject built. setup_messages injects a static "<count>" string per entry so
    // the cluster-debug toggle (F11) can label clusters with their size.
    const std::vector<std::pair<int, int>> &cluster_label_census();

    // Local player's world position (WorldChrMan chain, AOB-resolved + cached).
    // Returns false if not yet resolvable (early load) or the chain faulted.
    // For proximity clustering (v2). Coordinate space = global/world (verify).
    bool get_player_world_pos(float &x, float &y, float &z);

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

    // Per-section toggle banners. 7 display groups × {shown, hidden} = 14 static
    // rows at BASE..BASE+13. id = BASE + section*2 + (visible ? 0 : 1). The
    // names here are shared by the FMG injector (goblin_messages) and the toast.
    constexpr int TUTORIAL_FMG_ID_SECTION_BASE = 9004260;
    constexpr int TUTORIAL_SECTION_COUNT       = 7;
    inline constexpr const wchar_t *TUTORIAL_SECTION_NAMES[TUTORIAL_SECTION_COUNT] = {
        L"Equipment", L"Key Items", L"Loot", L"Magic", L"Quest", L"Reforged", L"World"};
    inline int section_toast_id(int section, bool visible)
    {
        return TUTORIAL_FMG_ID_SECTION_BASE + section * 2 + (visible ? 0 : 1);
    }

    // Inject the codex-toast TutorialParam rows for the F10/F9 banners. Rows
    // get menuType=0 (upper-left codex caption widget) with textId pointing
    // at TutorialBody.fmg entries injected by goblin_messages.
    // Returns true on success.
    bool inject_tutorial_popup_rows();

    // Fire an upper-left codex-style toast for one of the injected TutorialParam
    // rows (pass a TUTORIAL_FMG_ID_* id). Static text, no FMG rewrite — same
    // path as the F10 banner. Safe from any thread once init has run.
    void show_codex_toast(int tutorial_id);

    // Background thread polling the toggle hotkey.
    void toggle_hotkey_loop();

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the F10/gamepad personal show/hide and shows the toggle banner.
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

        // Persist the current section/category visibility to the ini (the menu's
        // Save button). Posts a request; the watcher thread does the file I/O.
        void request_save();

        bool clustering_available();       // false on builds without clustering data
        bool clusters_expanded();
        void set_clusters_expanded(bool expanded);
        bool cluster_debug();
        void set_cluster_debug(bool on);
    }
};
