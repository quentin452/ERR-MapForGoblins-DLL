#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace goblin
{
    // Create the ini from the schema defaults if missing; otherwise migrate it
    // in place (add new keys, apply renames, comment out obsolete keys),
    // preserving user-set values. Then load values into goblin::config.
    void load_config(const std::filesystem::path &ini_path);
    void ensure_ini(const std::filesystem::path &ini_path);

    // Write the current [Display Sections] section_* values back to the ini,
    // preserving the rest of the file. Called when the in-game section toggle
    // changes a group's visibility so the choice survives relaunch.
    void save_section_states(const std::filesystem::path &ini_path);

    // Persist EVERY bool config var (sections + the 63 categories + other
    // toggles) to the ini, preserving the rest of the file. The menu's Save
    // button calls this after syncing runtime visibility into the config vars.
    void save_all_bool_settings(const std::filesystem::path &ini_path);

    // Re-seed every config var from its schema default, then persist to the ini
    // (the menu's "Reset parameters to default" danger button). Does NOT touch
    // the live runtime visibility atomics — those are seeded at inject time, so
    // a game restart is needed to fully apply the defaults.
    void reset_to_defaults_and_save(const std::filesystem::path &ini_path);

    // The ini path last passed to load_config(), for code (e.g. the in-game
    // section toggle) that needs to persist back without threading the path.
    const std::filesystem::path &config_ini_path();

    namespace config
    {
        extern uint8_t loadDelay;
        extern bool requireMapFragments;
        extern bool collectedGraying;
        extern bool hideCollected;
        extern bool clusterDebugRadius;
        extern bool showRegionLabels; // overlay map: draw major-region names (Limgrave, Caelid, ...)
        extern bool diagLootFlags;    // one-shot [LOOTDIAG]: dump all candidate pickup flags per loot lot
        extern bool debugLogging;
        extern bool showAll;  // master switch: show every category (see
                              // is_category_enabled) except those listed below
        extern bool iconsHidden;  // persisted master off (menu 'Show icons' / F10)
        extern uint32_t overlayToggleKey;  // VK_* for the overlay menu toggle (default F1)
        extern std::string showAllExcept;  // comma-separated category names to
                              // keep hidden even when showAll is on (matched
                              // loosely against the category display name)

        // One bool per goblin::generated::Category, indexed by the enum value.
        // Replaces the former ~65 individual show* category flags; access via
        // category_config_ptr()/is_category_enabled() in goblin_inject.cpp.
        extern bool showCategory[];

        // World - Bosses
        extern bool hideKilledBosses;  // true=hide killed icons, false=green checkmark

        // Compatibility
        extern bool liveLootFlags;  // read live ItemLotParam getItemFlagId at runtime
                                    // → loot markers hide on the actual light-point
                                    // pickup for the current regulation (Randomizer-safe)
        extern bool liveLootLabels; // read live ItemLotParam item+category at runtime
                                    // → loot marker name shows the item the lot now
                                    // gives (Randomizer-safe). Needs full-band FMG copy.
        extern bool liveLootIcons;  // re-icon & re-gate loot markers by the LIVE item's
                                    // category at inject (so a randomized item shows its
                                    // own icon under its own show_* toggle).
        extern bool anonymousLoot;  // spoiler-free mode: every loot marker shows a
                                    // gray "?" icon + a generic localized label instead
                                    // of the real item (blind randomizer runs).
        extern bool projectDungeons; // remap minor-dungeon entries (catacombs/caves/
                                    // tunnels/hero's graves — areaNo with no in-game
                                    // map page) onto the overworld via the game's own
                                    // WorldMapLegacyConvParam (baked LEGACY_CONV), so
                                    // their icons become visible near the entrance.

        // ── ERR Markers ─────────────────────────────────────────────────
        // Patches to ERR's pre-placed WorldMapPointParam entries (camps,
        // merchants, field bosses, dungeon entrances). Each `patch*` flag
        // decides whether we rewrite that category's flags so the marker
        // appears/hides in sync with map-fragment discovery and (for
        // dungeons) boss completion. `false` = leave the row alone — the
        // icon still appears with whatever flags ERR ships.
        extern bool patchOverworldBossIcons;
        extern bool patchDungeonBossIcons;
        extern bool patchCampIcons;
        extern bool patchMerchantIcons;
        // Cosmetic options layered on top of the patch flags above. Each
        // requires its corresponding `patch*` flag to be true to take
        // effect (we only touch the icon when we're already rewriting
        // the row).
        extern bool redifyBossIcons;            // overworld bosses: red icon + auto-hide on kill
        extern bool redifyDungeonIcons;         // dungeon entrances: red icon
        extern bool hideDungeonIconsOnClear;    // dungeon entrances: hide on boss kill

        // Marker dump (hotkey → dump beacon/stamp coords to file)
        extern bool enableMarkerDump;
        extern uint32_t markerDumpKey;  // Win32 VK_* code (default VK_F9 = 0x78)

        // Thread 7 — coverage-gap observers (log to logs/MapForGoblins_events.log).
        // debugEventFlags  = hook SetEventFlag (every flag the game sets).
        // debugItemGrants  = hook AddItemFunc (every inventory grant / pickup).
        // See goblin_debug_events.{hpp,cpp}.
        extern bool debugEventFlags;
        extern bool debugItemGrants;
        // debugFlagCapture = light SetEventFlag hook for the overlay's NPC
        // death-flag capture tool (Quest Browser Part 2).
        extern bool debugFlagCapture;

        // Read-only world-map cursor probe (logs cursor coords to confirm the RE
        // offsets / marker-space). See goblin_worldmap_probe.{hpp,cpp}.
        extern bool debugWorldmapProbe;

        // Dev prototype: draw overlay-rendered marker dots projected onto the open
        // world map (verifies the world->screen affine). See goblin_overlay.cpp +
        // goblin_worldmap_probe::get_live_view.
        extern bool overlayMarkersProto;

        // When false, skip the native WorldMapPointParam injection (+ apply_map_logic)
        // so the ImGui overlay is the sole map source (no native page-build = no
        // freeze, no double-draw). Default true = classic native-map behaviour.
        extern bool nativeMapInjection;

        // Dev: log ER's render-output dims each ~2s ([RENDIMS]) to diagnose the
        // mid-session resolution-change zoom corruption. Read-only.
        extern bool debugRenderDims;

        // EXPERIMENTAL: on a swapchain resize, raw-poke ER's stale render-output dims
        // to the new size so a mid-session resolution change doesn't leave the world
        // zoomed (no restart needed). Same-aspect only. Default off.
        extern bool fixMidsessionResolution;

        // Overlay marker sizes. Final = resolution-base × master × type-scale.
        extern float overlayMasterScale;   // all overlay markers + piles
        extern float overlayIconScale;     // category marker icons
        extern float overlayClusterScale;  // cluster pile glyphs

        // Diagnostic: dump the native built-icon set (CSWorldMapPointMan+0x398) ids.
        extern bool dumpNativePins;

        // In-game minimap HUD (corner, north-up, overworld-only). Opt-in.
        extern bool showMinimap;
        extern float minimapZoom;     // px per world-unit
        extern float minimapSize;     // radius px
        extern float minimapOpacity;  // background opacity 0..1

        // EXPERIMENTAL live world-map icon refresh. Hooks the engine's placed-
        // map-point (re)build (FUN_140a82a80) so a section/category toggle re-renders
        // icons WHILE the map is open instead of only on the next open. Off by
        // default; needs in-game validation. See docs/windows_re_live_refresh_capture.md.
        extern bool liveRefreshWorldMap;

        // In-game per-section visibility (the 7 display groups). The section_*
        // bools are the persisted runtime state, driven live by the overlay menu
        // (F1) and written back on Save. See goblin_config_schema [Display Sections].
        extern bool sectionEquipment, sectionKeyItems, sectionLoot, sectionMagic,
                    sectionQuest, sectionReforged, sectionWorld;

        // Marker clustering (v1). See goblin_config_schema [Clustering].
        extern bool enableClustering;
        extern bool clusterHard;  // hard = mixed-category piles; soft = per-category
        extern uint8_t clusterThreshold;   // base cluster size; the FAR (clustered) size when distance-adaptive
        // Distance-adaptive clustering: near the player use a HIGH threshold (few
        // piles = detail / real items), ramping DOWN to clusterThreshold far away
        // (more clustering = fewer distant icons).
        extern bool    clusterDistanceAdaptive;
        extern uint8_t clusterNearThreshold; // detail size NEAR player (high = more individual items)
        extern uint8_t clusterNearRadius;    // tiles: full-detail radius
        extern uint8_t clusterFarRadius;     // tiles: at/beyond → clusterThreshold (clustered)
        // Per-category cluster opt-out. Comma-separated category names (loose match,
        // like showAllExcept) that stay EXACT markers and never fold into a cluster.
        // Empty = every category is clusterable (the v1 behaviour).
        extern std::string clusterExclude;
        // Per-category cluster-threshold overrides: "Name:N,Name2:M" (loose name
        // match). A category not listed uses the global clusterThreshold. Driven by
        // the per-category threshold inputs in the overlay.
        extern std::string clusterThresholdOverrides;

        // Thread 1 v1.5 — quest-aware quest-NPC markers. When true, a WorldQuestNPC
        // marker for one of the 34 curated questlines shows only while that quest is
        // active (its event flag set); off = all quest-NPC markers always shown.
        extern bool questNpcQuestAware;

        // Quest Browser per-step progress: one '0'/'1' char per global step index
        // (author order in goblin_quest_steps). Auto-grown; persisted on Save.
        extern std::string questProgress;

        // Quest Browser: grey out + tag a questline ([unfinishable]/[concluded])
        // when the overlay reads its NPC's death/conclusion fail_flag as set.
        // Default true (existing behaviour). EXPERIMENTAL — the per-NPC death
        // flags are reverse-engineered and may mis-grey; users can turn it off.
        // Toggle is in the overlay Quest Browser; persisted in the ini.
        extern bool questGreyOnDeath;
    };

    uint32_t parse_vk_code(std::string name);
    uint16_t parse_gamepad_combo(std::string s);
};
