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

    namespace config
    {
        extern uint8_t loadDelay;
        extern bool requireMapFragments;
        extern bool debugLogging;

        // Equipment
        extern bool showArmaments;
        extern bool showArmour;
        extern bool showAshesOfWar;
        extern bool showSpirits;
        extern bool showTalismans;

        // Key items
        extern bool showCelestialDew;
        extern bool showCookbooks;
        extern bool showCrystalTears;
        extern bool showImbuedSwordKeys;
        extern bool showLarvalTears;
        extern bool showScadutreeFragments;
        extern bool showGreatRunes;
        extern bool showLostAshes;
        extern bool showPotsNPerfumes;
        extern bool showSeedsTears;
        extern bool showWhetblades;

        // Loot
        extern bool showAmmo;
        extern bool showBellBearings;
        extern bool showMerchantBellBearings;
        extern bool showConsumables;
        extern bool showCraftingMaterials;
        extern bool showMPFingers;
        extern bool showMaterialNodes;
        extern bool showReusables;
        extern bool showSmithingStones;
        extern bool showSmithingStonesLow;
        extern bool showSmithingStonesRare;
        extern bool showGoldenRunes;
        extern bool showGoldenRunesLow;
        extern bool showStoneswordKeys;
        extern bool showThrowables;
        extern bool showPrattlingPates;
        extern bool showRuneArcs;
        extern bool showDragonHearts;
        extern bool showGloveworts;
        extern bool showGreatGloveworts;
        extern bool showRadaFruit;
        extern bool showGestures;
        extern bool showGreases;
        extern bool showUtilities;
        extern bool showStatBoosts;
        extern bool showFortunes;
        extern bool showHostileNPC;

        // Magic
        extern bool showIncantations;
        extern bool showMemoryStones;
        extern bool showPrayerbooks;
        extern bool showSorceries;

        // World - Bosses
        extern bool showBosses;
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

        // Quest
        extern bool showDeathroot;
        extern bool showProgression;
        extern bool showSeedbedCurses;

        // Reforged
        extern bool showEmberPieces;
        extern bool showItemsAndChanges;
        extern bool showRunePieces;

        // World
        extern bool showGraces;
        extern bool showImpStatues;
        extern bool showWorldMaps;
        extern bool showPaintings;
        extern bool showSpiritSprings;
        extern bool showSpiritspringHawks;
        extern bool showStakesOfMarika;
        extern bool showSummoningPools;
        extern bool showKindlingSpirits;
        extern bool showInteractables;

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

        // ERSC-hosting workaround: hotkey toggles WorldMapPointParam +
        // PlaceName FMG between vanilla and expanded states. Press before
        // hosting a co-op session, press again after.
        extern bool enableToggleHotkey;
        extern uint32_t toggleInjectionKey;  // default VK_F10 = 0x79
        // XInput button bitmask combo. All buttons in the mask must be
        // held simultaneously to fire. Default Y + R3 (right stick click).
        extern uint16_t toggleGamepadMask;
    };

    uint32_t parse_vk_code(std::string name);
    uint16_t parse_gamepad_combo(std::string s);
};
