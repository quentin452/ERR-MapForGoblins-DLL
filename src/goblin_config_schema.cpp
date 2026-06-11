// AUTHORITATIVE ini layout — see goblin_config_schema.hpp.
// Dependency-free (no spdlog / mINI) so tools/mfg_inigen can link just this.

#include "goblin_config_schema.hpp"
#include "goblin_config.hpp"

#include <cstring>

// ── config variable definitions ─────────────────────────────────────────
// Initial values are seeded from the schema defaults by apply_defaults() at
// load time; the literals here are just a sane fallback.
namespace goblin::config
{
    uint8_t loadDelay = 5;
    bool requireMapFragments = true;
    bool debugLogging = false;

    bool showArmaments = false, showArmour = false, showAshesOfWar = false,
         showSpirits = false, showTalismans = false;

    bool showCelestialDew = false, showCookbooks = false, showCrystalTears = false,
         showGreatRunes = false, showImbuedSwordKeys = false, showLarvalTears = false,
         showLostAshes = false, showPotsNPerfumes = false, showScadutreeFragments = false,
         showSeedsTears = false, showWhetblades = false;

    bool showAmmo = false, showBellBearings = false, showMerchantBellBearings = false,
         showConsumables = false, showGreases = false, showUtilities = false,
         showStatBoosts = false, showCraftingMaterials = false, showGloveworts = false,
         showGoldenRunes = false, showGoldenRunesLow = false, showGreatGloveworts = false,
         showMaterialNodes = false, showMPFingers = false, showPrattlingPates = false,
         showRadaFruit = false, showGestures = false, showReusables = false,
         showSmithingStones = false, showSmithingStonesLow = false,
         showSmithingStonesRare = false, showStoneswordKeys = false,
         showThrowables = false, showRuneArcs = false, showDragonHearts = false;

    bool showIncantations = false, showMemoryStones = false, showPrayerbooks = false,
         showSorceries = false;

    bool showDeathroot = false, showProgression = false, showSeedbedCurses = false;

    bool showEmberPieces = true, showItemsAndChanges = false, showFortunes = false,
         showRunePieces = true;

    bool showBosses = false, showGraces = false, showHostileNPC = false,
         showImpStatues = false, showPaintings = false, showSpiritSprings = false,
         showSpiritspringHawks = false, showStakesOfMarika = false,
         showSummoningPools = false, showKindlingSpirits = true,
         showInteractables = false, showWorldMaps = false, hideKilledBosses = false;

    bool patchOverworldBossIcons = true, patchDungeonBossIcons = true,
         patchCampIcons = true, patchMerchantIcons = true,
         redifyBossIcons = false, redifyDungeonIcons = false,
         hideDungeonIconsOnClear = false;

    bool enableMarkerDump = false;
    uint32_t markerDumpKey = 0x78; // VK_F9
    bool enableToggleHotkey = true;
    uint32_t toggleInjectionKey = 0x79; // VK_F10
    uint16_t toggleGamepadMask = 0x8000 | 0x0080; // Y + R3
}

// ── schema ───────────────────────────────────────────────────────────────
namespace
{
    using goblin::IniEntry;
    using goblin::IniSection;
    using goblin::IniType;
    namespace cfg = goblin::config;

    constexpr bool ERR = true; // err-only marker for readability

    // helper macros to keep the table compact
#define B(k, var, def, cmt) IniEntry{k, IniType::Bool, &cfg::var, def, cmt, false, nullptr}
#define BE(k, var, def, cmt) IniEntry{k, IniType::Bool, &cfg::var, def, cmt, true, nullptr}

    std::vector<IniSection> build_schema()
    {
        return {
            {"Goblin", nullptr, false, {
                IniEntry{"load_delay", IniType::U8, &cfg::loadDelay, "5",
                         "Delay in seconds before loading map icons (wait for game initialization)", false, nullptr},
                B("require_map_fragments", requireMapFragments, "true",
                  "Require map fragment discovery before showing icons in that area"),
                B("debug_logging", debugLogging, "false",
                  "Enable verbose debug logging (memory addresses, param details, FMG internals)"),
            }},

            {"Equipment", nullptr, false, {
                B("show_armaments", showArmaments, "false", "Weapons, shields, bows, staves, etc."),
                B("show_armour", showArmour, "false", "Armor pieces (helms, chest, gauntlets, legs)"),
                B("show_ashes_of_war", showAshesOfWar, "false", "Ashes of War (weapon skills)"),
                B("show_spirits", showSpirits, "false", "Spirit Ashes (summons)"),
                B("show_talismans", showTalismans, "false", "Talismans"),
            }},

            {"Key Items", nullptr, false, {
                B("show_celestial_dew", showCelestialDew, "false", "Celestial Dew (for reversing Spirit Ashes upgrades)"),
                B("show_cookbooks", showCookbooks, "false", "Cookbooks (crafting recipes)"),
                B("show_crystal_tears", showCrystalTears, "false", "Crystal Tears (for Flask of Wondrous Physick)"),
                B("show_great_runes", showGreatRunes, "false", "Great Runes (dropped by story bosses)"),
                B("show_imbued_sword_keys", showImbuedSwordKeys, "false", "Imbued Sword Keys (Four Belfries)"),
                B("show_larval_tears", showLarvalTears, "false", "Larval Tears (respec items)"),
                B("show_lost_ashes", showLostAshes, "false", "Lost Ashes of War"),
                B("show_pots_n_perfumes", showPotsNPerfumes, "false", "Cracked Pots, Ritual Pots, Perfume Bottles"),
                B("show_scadutree_fragments", showScadutreeFragments, "false", "Scadutree Fragments (DLC blessing upgrade)"),
                B("show_seeds_tears", showSeedsTears, "false", "Golden Seeds, Sacred Tears, Revered Spirit Ashes"),
                B("show_whetblades", showWhetblades, "false", "Whetblades (weapon infusion types)"),
            }},

            {"Loot", nullptr, false, {
                B("show_ammo", showAmmo, "false", "Arrows, bolts, greatarrows, greatbolts"),
                B("show_bell_bearings", showBellBearings, "false", "Bell Bearings from treasures/chests/quest rewards"),
                B("show_merchant_bell_bearings", showMerchantBellBearings, "false",
                  "Bell Bearings dropped by KILLING merchants (Kale, Patches, Gostoc, nomadic\nmerchants, etc.) - off by default; this is a kill-the-merchant reward"),
                B("show_consumables", showConsumables, "false", "Healing/buff consumables (boluses, cured meats, livers)"),
                B("show_greases", showGreases, "false", "Weapon greases"),
                B("show_utilities", showUtilities, "false", "Utility items (rainbow stone, glowstone, soap, soft cotton)"),
                B("show_stat_boosts", showStatBoosts, "false", "Stat-up items (Starlight Shards, Sacrificial Twig, Blessing of Marika)"),
                B("show_crafting_materials", showCraftingMaterials, "false", "Crafting materials (flowers, bones, bugs, etc.)"),
                B("show_gloveworts", showGloveworts, "false", "Gloveworts (Grave/Ghost [1-9]) - Spirit Ash upgrade materials"),
                B("show_golden_runes", showGoldenRunes, "false", "Golden Runes [4000+], Hero's/Numen's/Lord's/Shadow Realm Runes"),
                B("show_golden_runes_low", showGoldenRunesLow, "false", "Golden Runes [200-3000], Broken Runes (disabled by default - many icons)"),
                B("show_great_gloveworts", showGreatGloveworts, "false", "Great Gloveworts (Great Grave, Great Ghost) - rare boss drops"),
                B("show_material_nodes", showMaterialNodes, "false", "One-time gathering nodes (Erdleaf Flower, Trina's Lily, etc.)"),
                B("show_mp_fingers", showMPFingers, "false", "Multiplayer items (Furlcalling/Wizened Fingers, Recusant/Bloody Finger)"),
                B("show_prattling_pates", showPrattlingPates, "false", "Prattling Pates"),
                B("show_rada_fruit", showRadaFruit, "false", "Rada Fruit (DLC stat-up consumable)"),
                B("show_gestures", showGestures, "false", "Gestures (auto-discovered via gesture template 90005570 - covers 7/9 spawns)"),
                B("show_reusables", showReusables, "false", "Reusable tools (Mimic Veil, Margit's Shackle, etc.)"),
                B("show_smithing_stones", showSmithingStones, "false", "Smithing Stones [7-8], Somber [7-9], Scadushards"),
                B("show_smithing_stones_low", showSmithingStonesLow, "false", "Smithing Stones [1-6], Somber [1-6] (disabled by default - many icons)"),
                B("show_smithing_stones_rare", showSmithingStonesRare, "false", "Ancient Dragon Smithing Stones (rare, endgame)"),
                B("show_stonesword_keys", showStoneswordKeys, "false", "Stonesword Keys"),
                B("show_throwables", showThrowables, "false", "Throwable items (darts, daggers, stones, chakrams, warming stones)"),
                B("show_rune_arcs", showRuneArcs, "false", "Rune Arcs (buffs for active Great Rune)"),
                B("show_dragon_hearts", showDragonHearts, "false", "Dragon Hearts (for Dragon Communion incantations)"),
            }},

            {"Magic", nullptr, false, {
                B("show_incantations", showIncantations, "false", "Incantation locations"),
                B("show_memory_stones", showMemoryStones, "false", "Memory Stone locations (extra spell slots)"),
                B("show_prayerbooks", showPrayerbooks, "false", "Prayerbooks and Scrolls (unlock spells at vendors)"),
                B("show_sorceries", showSorceries, "false", "Sorcery locations"),
            }},

            {"Quest", nullptr, false, {
                B("show_deathroot", showDeathroot, "false", "Deathroot locations (for Gurranq)"),
                B("show_progression", showProgression, "false", "Quest progression items (medallions, keys, Needles, quest-specific goods)"),
                B("show_seedbed_curses", showSeedbedCurses, "false", "Seedbed Curse locations (for Dung Eater quest)"),
            }},

            {"Reforged",
             "Elden Ring Reforged-only content. Absent from the vanilla build.",
             ERR, {
                BE("show_ember_pieces", showEmberPieces, "true", "ERR Ember Piece locations"),
                BE("show_items_and_changes", showItemsAndChanges, "false", "ERR-added items: Oracle Effigy/Remedy, Starlight Tokens, Sealed Curios"),
                BE("show_fortunes", showFortunes, "false", "ERR Fortune trinkets (12 types)"),
                BE("show_rune_pieces", showRunePieces, "true", "ERR Rune Piece locations"),
            }},

            {"World", nullptr, false, {
                B("show_bosses", showBosses, "false", "Boss markers (field bosses, dungeon bosses)"),
                B("show_graces", showGraces, "false", "Sites of Grace"),
                B("show_hostile_npc", showHostileNPC, "false", "Hostile NPC invader spawn locations (auto-discovered via teamType 24/27)"),
                B("show_imp_statues", showImpStatues, "false", "Imp Statue (Stonesword Key fog gate) locations"),
                B("show_paintings", showPaintings, "false", "Painting locations"),
                B("show_spirit_springs", showSpiritSprings, "false", "Spirit Spring (horse jump) locations"),
                B("show_spiritspring_hawks", showSpiritspringHawks, "false", "Spiritspring Hawk locations"),
                B("show_stakes_of_marika", showStakesOfMarika, "false", "Stakes of Marika (respawn points)"),
                B("show_summoning_pools", showSummoningPools, "false", "Summoning Pool (Martyr Effigy) locations"),
                BE("show_kindling_spirits", showKindlingSpirits, "true",
                   "ERR Kindling Spirits in Misty Forest (m60_45_37_00) - collect all 5\nbetween rests for the Kindling Spirit incantation. Markers hide\npermanently once the incantation is acquired (engine flag 1045377500),\nand individually within a run via SFX-region runtime state."),
                B("show_interactables", showInteractables, "false",
                  "Interactive world objects & puzzles. Includes:\n  - Blue seal puzzles (~65 seals across the overworld, unlock hidden cellars)\n  - \"Light flame\" interacts: Sellia chalices (3), Snow Town seal-release\n    statues (4), Siofra River lanterns (~14)\n  - Hero's Tomb direction statues (16, point at hidden Hero's Tomb caves)\nEach marker hides on activation via its own engine flag."),
                B("show_world_maps", showWorldMaps, "false", "World Map fragment locations"),
                B("hide_killed_bosses", hideKilledBosses, "false", "Hide boss/invader/hawk markers after defeat (false = show green checkmark instead)"),
            }},

            {"ERR Markers",
             "This section applies this mod's display rules to ERR's OWN pre-placed map\n"
             "markers (camps, merchants, bosses, dungeon entrances) - NOT the icons this\n"
             "mod injects - so both icon sets follow the same visibility logic\n"
             "(map-fragment discovery, hide on clear). Disable a toggle to leave that\n"
             "marker group exactly as ERR ships it. Our own boss markers are\n"
             "[World] show_bosses and independent of this section.",
             ERR, {
                BE("patch_overworld_boss_icons", patchOverworldBossIcons, "true",
                   "Tie ERR-placed overworld field-boss markers (textId2=5100) to\nour map-fragment discovery flag. Combine with redify_boss_icons below."),
                BE("patch_dungeon_boss_icons", patchDungeonBossIcons, "true",
                   "Tie ERR-placed dungeon/cave entrance markers (textId3=5100/5300) to\nour map-fragment discovery flag. Required for redify/hide_dungeon below."),
                BE("patch_camp_icons", patchCampIcons, "true",
                   "Tie ERR-placed enemy camp markers (textId2=5000) to our discovery flag."),
                BE("patch_merchant_icons", patchMerchantIcons, "true",
                   "Tie ERR-placed merchant markers (textId4=8800) to our discovery flag."),
                BE("redify_boss_icons", redifyBossIcons, "false",
                   "Cosmetic: when patching overworld bosses, recolour iconId to 374 (red) AND\nauto-hide once the boss is defeated. Requires patch_overworld_boss_icons."),
                BE("redify_dungeon_icons", redifyDungeonIcons, "false",
                   "Cosmetic: when patching dungeon entrances, recolour iconId to 374.\nRequires patch_dungeon_boss_icons."),
                BE("hide_dungeon_icons_on_clear", hideDungeonIconsOnClear, "false",
                   "When patching dungeon entrances, hide the marker once the boss inside is\ndefeated. Requires patch_dungeon_boss_icons."),
            }},

            {"Debug",
             "Hotkeys: in-memory marker dump, and the injection toggle (workaround for\nhosting Seamless Co-op). Key names: F1-F24, A-Z, 0-9, Space, Escape, Tab,\nEnter, Backspace, Home, End, PageUp, PageDown, Insert, Delete, arrows.",
             false, {
                B("enable_marker_dump", enableMarkerDump, "false", "Master switch for the marker dump hotkey"),
                IniEntry{"marker_dump_key", IniType::VkKey, &cfg::markerDumpKey, "F9",
                         "Key to dump decoded markers to logs/MapForGoblins_markers.log. Default: F9.", false, nullptr},
                B("enable_toggle_hotkey", enableToggleHotkey, "true",
                  "Injection toggle: revert WorldMapPointParam + PlaceName FMG to vanilla and\nback (press once before hosting Seamless Co-op, again after)."),
                IniEntry{"toggle_injection_key", IniType::VkKey, &cfg::toggleInjectionKey, "F10",
                         "Keyboard key for the injection toggle. Default: F10.", false, nullptr},
                IniEntry{"toggle_gamepad_combo", IniType::GamepadMask, &cfg::toggleGamepadMask, "Y+R3",
                         "Gamepad combo (tokens joined with '+'): A,B,X,Y,LB,RB,L3/LSTICK,R3/RSTICK,\nBACK/SELECT/VIEW,START/MENU,UP/DOWN/LEFT/RIGHT. All held together. Default: Y+R3.", false, nullptr},
            }},
        };
    }

#undef B
#undef BE
}

const std::vector<goblin::IniSection> &goblin::ini_schema()
{
    static const std::vector<IniSection> schema = build_schema();
    return schema;
}

static void emit_comment(std::ostream &out, const char *c)
{
    if (!c) return;
    const char *s = c;
    while (*s)
    {
        const char *nl = std::strchr(s, '\n');
        if (nl)
        {
            out << "; ";
            out.write(s, nl - s);
            out << "\n";
            s = nl + 1;
        }
        else
        {
            out << "; " << s << "\n";
            break;
        }
    }
}

void goblin::emit_ini(std::ostream &out, bool include_err_only, const IniValueResolver &resolve)
{
    out << "; MapForGoblins configuration. Auto-generated from the in-code schema;\n"
        << "; the DLL re-syncs this file on launch (adds new keys, comments out removed ones).\n";
    for (auto const &sec : ini_schema())
    {
        if (sec.err_only && !include_err_only) continue;
        out << "\n";
        emit_comment(out, sec.comment);
        out << "[" << sec.name << "]\n";
        for (auto const &e : sec.entries)
        {
            if (e.err_only && !include_err_only) continue;
            emit_comment(out, e.comment);
            std::string val;
            if (!(resolve && resolve(sec.name, e, val))) val = e.def;
            out << e.key << " = " << val << "\n";
        }
    }
}
