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

    // The ini path last passed to load_config(), for code (e.g. the in-game
    // section toggle) that needs to persist back without threading the path.
    const std::filesystem::path &config_ini_path();

    namespace config
    {
        extern uint8_t loadDelay;
        extern bool requireMapFragments;
        extern bool debugLogging;
        extern bool showAll;  // master switch: show every category (see
                              // is_category_enabled) except those listed below
        extern bool iconsHidden;  // persisted master off (menu 'Show icons' / F10)
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

        // Thread 7 — SetEventFlag observer (logs every event flag the game sets to
        // logs/MapForGoblins_events.log). Coverage-gap discovery aid. See
        // goblin_debug_events.{hpp,cpp}.
        extern bool debugEventFlags;

        // In-game per-section visibility (the 7 display groups). The section_*
        // bools are the persisted runtime state, driven live by the overlay menu
        // (F1) and written back on Save. See goblin_config_schema [Display Sections].
        extern bool sectionEquipment, sectionKeyItems, sectionLoot, sectionMagic,
                    sectionQuest, sectionReforged, sectionWorld;

        // Marker clustering (v1). See goblin_config_schema [Clustering].
        extern bool enableClustering;
        extern uint8_t clusterThreshold;   // bucket clusters only if it holds > this many
    };

    uint32_t parse_vk_code(std::string name);
    uint16_t parse_gamepad_combo(std::string s);
};
