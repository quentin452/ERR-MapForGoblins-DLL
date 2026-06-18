// AUTHORITATIVE ini layout — see goblin_config_schema.hpp.
// Dependency-free (no spdlog / mINI) so tools/mfg_inigen can link just this.

#include "goblin_config_schema.hpp"
#include "goblin_config.hpp"
#include "goblin_map_data.hpp"

#include <cstring>

// ── config variable definitions ─────────────────────────────────────────
// Initial values are seeded from the schema defaults by apply_defaults() at
// load time; the literals here are just a sane fallback.
namespace goblin::config
{
    uint8_t loadDelay = 5;
    bool requireMapFragments = true;
    bool debugLogging = false;
    bool projectDungeons = true;
    bool showAll = false;
    bool iconsHidden = false;  // master off persisted (menu/F10 "Show icons")
    uint32_t overlayToggleKey = 0x70;  // VK_F1 — overlay menu open/close key
    std::string showAllExcept = "";

    // One bool per goblin::generated::Category, indexed by the enum value. Seeded
    // from the schema defaults by apply_defaults() at load time (the per-category
    // "true"/"false" defaults live on the B(...) entries below); the zero-init
    // here is just a sane fallback before load_config() runs.
    static constexpr int NUM_CATEGORIES =
        static_cast<int>(goblin::generated::Category::WorldInteractables) + 1;
    bool showCategory[NUM_CATEGORIES] = {};

    bool hideKilledBosses = false;

    // Live-loot / randomizer-compat options default ON for the plain VANILLA
    // build only (that's what Item/Enemy Randomizer players use) and OFF for ERR
    // and Convergence (opt-in — they add memory/CPU with no benefit without a
    // regulation mod). MFG_PROFILE_VANILLA is set by CMake for the vanilla bake
    // only (MFG_VANILLA covers vanilla AND convergence, so it can't be used here).
#ifdef MFG_PROFILE_VANILLA
    bool liveLootFlags = true, liveLootLabels = true, liveLootIcons = true;
#else
    bool liveLootFlags = false, liveLootLabels = false, liveLootIcons = false;
#endif
    bool anonymousLoot = false;  // opt-in spoiler-free mode (all profiles default off)

    bool patchOverworldBossIcons = true, patchDungeonBossIcons = true,
         patchCampIcons = true, patchMerchantIcons = true,
         redifyBossIcons = false, redifyDungeonIcons = false,
         hideDungeonIconsOnClear = false;

    bool enableMarkerDump = false;
    uint32_t markerDumpKey = 0x78; // VK_F9
    bool debugEventFlags = false;
    bool debugItemGrants = false;

    // In-game per-section visibility (the 7 display groups). Persisted so an
    // in-game toggle survives relaunch. Default all-visible = no behaviour change.
    bool sectionEquipment = true, sectionKeyItems = true, sectionLoot = true,
         sectionMagic = true, sectionQuest = true, sectionReforged = true,
         sectionWorld = true;

    // Marker clustering (v1, density-triggered, static). Collapses dense marker
    // piles into one cluster icon to cut the per-page map-open cost. Opt-in.
    bool enableClustering = false;
    uint8_t clusterThreshold = 8;     // a bucket clusters only if it holds > this many markers
    bool questNpcQuestAware = false;  // gate quest-NPC markers on quest-active flags
}

// ── schema ───────────────────────────────────────────────────────────────
namespace
{
    using goblin::IniEntry;
    using goblin::IniSection;
    using goblin::IniType;
    namespace cfg = goblin::config;
    using Cat = goblin::generated::Category;

    constexpr bool ERR = true; // err-only marker for readability

    // Default string for the live-loot options — "true" only in the vanilla
    // bake (see the var defs above), "false" elsewhere. Must match the compiled
    // bool defaults so the generated ini and the DLL agree.
#ifdef MFG_PROFILE_VANILLA
#define MFG_LL_DEF "true"
#else
#define MFG_LL_DEF "false"
#endif

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
                B("project_dungeons", projectDungeons, "true",
                  "Remap minor-dungeon icons (catacombs, caves, tunnels, hero's graves)\nonto the overworld map near their entrance. ER has no map page for them,\nso without this their icons are injected but never rendered."),
                B("show_all", showAll, "false",
                  "Master switch: show EVERY category at once, ignoring the individual\nshow_* toggles below. Quick way to reveal everything without flipping ~60 flags."),
                B("icons_hidden", iconsHidden, "false",
                  "Start with all map icons hidden (the menu's 'Show icons' master / F10).\nWritten back when you Save in-game, so the master on/off persists."),
                IniEntry{"show_all_except", IniType::String, &cfg::showAllExcept, "",
                         "When show_all is on, categories listed here stay hidden (comma-separated,\n"
                         "matched loosely vs the category name, e.g. SmithingStonesLow, GoldenRunesLow).",
                         false, nullptr},
                IniEntry{"overlay_toggle_key", IniType::VkKey, &cfg::overlayToggleKey, "F1",
                         "Key that opens/closes the in-game overlay menu. Default: F1.\n"
                         "Key names: F1-F24, A-Z, 0-9, Space, Escape, Tab, Enter, Home, End,\n"
                         "PageUp, PageDown, Insert, Delete, arrows.", false, nullptr},
            }},

            {"Display Sections",
             "In-game group visibility. These mirror the [Equipment]/[Key Items]/[Loot]/\n"
             "[Magic]/[Quest]/[Reforged]/[World] groups below: a show_* flag decides if a\n"
             "family's icons are loaded at all; the matching section flag here shows/hides\n"
             "those loaded icons as a group, and can be flipped live in-game without a\n"
             "restart from the overlay menu (F1). The value is written back here when you\n"
             "toggle and Save in-game, so it persists.",
             false, {
                B("section_equipment", sectionEquipment, "true", "Show the Equipment group's icons."),
                B("section_key_items", sectionKeyItems, "true", "Show the Key Items group's icons."),
                B("section_loot",      sectionLoot,      "true", "Show the Loot group's icons."),
                B("section_magic",     sectionMagic,     "true", "Show the Magic group's icons."),
                B("section_quest",     sectionQuest,     "true", "Show the Quest group's icons."),
                B("section_reforged",  sectionReforged,  "true", "Show the Reforged group's icons."),
                B("section_world",     sectionWorld,     "true", "Show the World group's icons."),
            }},

            {"Clustering",
             "Collapses dense piles of markers into a single cluster icon, so the world\n"
             "map opens fast even with everything enabled (the open cost scales with how\n"
             "many markers are on the displayed page). Density-triggered: only spots with\n"
             "more than the threshold of markers cluster; sparse markers stay exact.",
             false, {
                B("enable_clustering", enableClustering, "false",
                  "Master switch for marker clustering."),
                IniEntry{"cluster_threshold", IniType::U8, &cfg::clusterThreshold, "8",
                         "A location clusters only if it holds MORE than this many markers.\n"
                         "Lower = more aggressive clustering (faster, less precise).", false, nullptr},
            }},

            {"Equipment", nullptr, false, {
                B("show_armaments", showCategory[static_cast<int>(Cat::EquipArmaments)], "false", "Weapons, shields, bows, staves, etc."),
                B("show_armour", showCategory[static_cast<int>(Cat::EquipArmour)], "false", "Armor pieces (helms, chest, gauntlets, legs)"),
                B("show_ashes_of_war", showCategory[static_cast<int>(Cat::EquipAshesOfWar)], "false", "Ashes of War (weapon skills)"),
                B("show_spirits", showCategory[static_cast<int>(Cat::EquipSpirits)], "false", "Spirit Ashes (summons)"),
                B("show_talismans", showCategory[static_cast<int>(Cat::EquipTalismans)], "false", "Talismans"),
            }},

            {"Key Items", nullptr, false, {
                B("show_celestial_dew", showCategory[static_cast<int>(Cat::KeyCelestialDew)], "false", "Celestial Dew (for reversing Spirit Ashes upgrades)"),
                B("show_cookbooks", showCategory[static_cast<int>(Cat::KeyCookbooks)], "false", "Cookbooks (crafting recipes)"),
                B("show_crystal_tears", showCategory[static_cast<int>(Cat::KeyCrystalTears)], "false", "Crystal Tears (for Flask of Wondrous Physick)"),
                B("show_great_runes", showCategory[static_cast<int>(Cat::KeyGreatRunes)], "false", "Great Runes (dropped by story bosses)"),
                B("show_imbued_sword_keys", showCategory[static_cast<int>(Cat::KeyImbuedSwordKeys)], "false", "Imbued Sword Keys (Four Belfries)"),
                B("show_larval_tears", showCategory[static_cast<int>(Cat::KeyLarvalTears)], "false", "Larval Tears (respec items)"),
                B("show_lost_ashes", showCategory[static_cast<int>(Cat::KeyLostAshes)], "false", "Lost Ashes of War"),
                B("show_pots_n_perfumes", showCategory[static_cast<int>(Cat::KeyPotsNPerfumes)], "false", "Cracked Pots, Ritual Pots, Perfume Bottles"),
                B("show_scadutree_fragments", showCategory[static_cast<int>(Cat::KeyScadutreeFragments)], "false", "Scadutree Fragments (DLC blessing upgrade)"),
                B("show_seeds_tears", showCategory[static_cast<int>(Cat::KeySeedsTears)], "false", "Golden Seeds, Sacred Tears, Revered Spirit Ashes"),
                B("show_whetblades", showCategory[static_cast<int>(Cat::KeyWhetblades)], "false", "Whetblades (weapon infusion types)"),
            }},

            {"Loot", nullptr, false, {
                B("show_ammo", showCategory[static_cast<int>(Cat::LootAmmo)], "false", "Arrows, bolts, greatarrows, greatbolts"),
                B("show_bell_bearings", showCategory[static_cast<int>(Cat::LootBellBearings)], "false", "Bell Bearings from treasures/chests/quest rewards"),
                B("show_merchant_bell_bearings", showCategory[static_cast<int>(Cat::LootMerchantBellBearings)], "false",
                  "Bell Bearings dropped by KILLING merchants (Kale, Patches, Gostoc, nomadic\nmerchants, etc.) - off by default; this is a kill-the-merchant reward"),
                B("show_consumables", showCategory[static_cast<int>(Cat::LootConsumables)], "false", "Healing/buff consumables (boluses, cured meats, livers)"),
                B("show_greases", showCategory[static_cast<int>(Cat::LootGreases)], "false", "Weapon greases"),
                B("show_utilities", showCategory[static_cast<int>(Cat::LootUtilities)], "false", "Utility items (rainbow stone, glowstone, soap, soft cotton)"),
                B("show_stat_boosts", showCategory[static_cast<int>(Cat::LootStatBoosts)], "false", "Stat-up items (Starlight Shards, Sacrificial Twig, Blessing of Marika)"),
                B("show_crafting_materials", showCategory[static_cast<int>(Cat::LootCraftingMaterials)], "false", "Crafting materials (flowers, bones, bugs, etc.)"),
                B("show_gloveworts", showCategory[static_cast<int>(Cat::LootGloveworts)], "false", "Gloveworts (Grave/Ghost [1-9]) - Spirit Ash upgrade materials"),
                B("show_golden_runes", showCategory[static_cast<int>(Cat::LootGoldenRunes)], "false", "Golden Runes [4000+], Hero's/Numen's/Lord's/Shadow Realm Runes"),
                B("show_golden_runes_low", showCategory[static_cast<int>(Cat::LootGoldenRunesLow)], "false", "Golden Runes [200-3000], Broken Runes (disabled by default - many icons)"),
                B("show_great_gloveworts", showCategory[static_cast<int>(Cat::LootGreatGloveworts)], "false", "Great Gloveworts (Great Grave, Great Ghost) - rare boss drops"),
                B("show_material_nodes", showCategory[static_cast<int>(Cat::LootMaterialNodes)], "false", "One-time gathering nodes (Erdleaf Flower, Trina's Lily, etc.)"),
                B("show_mp_fingers", showCategory[static_cast<int>(Cat::LootMPFingers)], "false", "Multiplayer items (Furlcalling/Wizened Fingers, Recusant/Bloody Finger)"),
                B("show_prattling_pates", showCategory[static_cast<int>(Cat::LootPrattlingPates)], "false", "Prattling Pates"),
                B("show_rada_fruit", showCategory[static_cast<int>(Cat::LootRadaFruit)], "false", "Rada Fruit (DLC stat-up consumable)"),
                B("show_gestures", showCategory[static_cast<int>(Cat::LootGestures)], "false", "Gestures (auto-discovered via gesture template 90005570 - covers 7/9 spawns)"),
                B("show_reusables", showCategory[static_cast<int>(Cat::LootReusables)], "false", "Reusable tools (Mimic Veil, Margit's Shackle, etc.)"),
                B("show_smithing_stones", showCategory[static_cast<int>(Cat::LootSmithingStones)], "false", "Smithing Stones [7-8], Somber [7-9], Scadushards"),
                B("show_smithing_stones_low", showCategory[static_cast<int>(Cat::LootSmithingStonesLow)], "false", "Smithing Stones [1-6], Somber [1-6] (disabled by default - many icons)"),
                B("show_smithing_stones_rare", showCategory[static_cast<int>(Cat::LootSmithingStonesRare)], "false", "Ancient Dragon Smithing Stones (rare, endgame)"),
                B("show_stonesword_keys", showCategory[static_cast<int>(Cat::LootStoneswordKeys)], "false", "Stonesword Keys"),
                B("show_throwables", showCategory[static_cast<int>(Cat::LootThrowables)], "false", "Throwable items (darts, daggers, stones, chakrams, warming stones)"),
                B("show_rune_arcs", showCategory[static_cast<int>(Cat::LootRuneArcs)], "false", "Rune Arcs (buffs for active Great Rune)"),
                B("show_dragon_hearts", showCategory[static_cast<int>(Cat::LootDragonHearts)], "false", "Dragon Hearts (for Dragon Communion incantations)"),
            }},

            {"Magic", nullptr, false, {
                B("show_incantations", showCategory[static_cast<int>(Cat::MagicIncantations)], "false", "Incantation locations"),
                B("show_memory_stones", showCategory[static_cast<int>(Cat::MagicMemoryStones)], "false", "Memory Stone locations (extra spell slots)"),
                B("show_prayerbooks", showCategory[static_cast<int>(Cat::MagicPrayerbooks)], "false", "Prayerbooks and Scrolls (unlock spells at vendors)"),
                B("show_sorceries", showCategory[static_cast<int>(Cat::MagicSorceries)], "false", "Sorcery locations"),
            }},

            {"Quest", nullptr, false, {
                B("show_deathroot", showCategory[static_cast<int>(Cat::QuestDeathroot)], "false", "Deathroot locations (for Gurranq)"),
                B("show_progression", showCategory[static_cast<int>(Cat::QuestProgression)], "false", "Quest progression items (medallions, keys, Needles, quest-specific goods)"),
                B("show_seedbed_curses", showCategory[static_cast<int>(Cat::QuestSeedbedCurses)], "false", "Seedbed Curse locations (for Dung Eater quest)"),
            }},

            {"Reforged",
             "Elden Ring Reforged-only content. Absent from the vanilla build.",
             ERR, {
                BE("show_ember_pieces", showCategory[static_cast<int>(Cat::ReforgedEmberPieces)], "true", "ERR Ember Piece locations"),
                BE("show_items_and_changes", showCategory[static_cast<int>(Cat::ReforgedItemsAndChanges)], "false", "ERR-added items: Oracle Effigy/Remedy, Starlight Tokens, Sealed Curios"),
                BE("show_fortunes", showCategory[static_cast<int>(Cat::ReforgedFortunes)], "false", "ERR Fortune trinkets (12 types)"),
                BE("show_rune_pieces", showCategory[static_cast<int>(Cat::ReforgedRunePieces)], "true", "ERR Rune Piece locations"),
            }},

            {"World", nullptr, false, {
                B("show_bosses", showCategory[static_cast<int>(Cat::WorldBosses)], "false", "Boss markers (field bosses, dungeon bosses)"),
                B("show_graces", showCategory[static_cast<int>(Cat::WorldGraces)], "false", "Sites of Grace"),
                B("show_hostile_npc", showCategory[static_cast<int>(Cat::WorldHostileNPC)], "false", "Hostile NPC invader spawn locations (auto-discovered via teamType 24/27)"),
                B("show_quest_npc", showCategory[static_cast<int>(Cat::WorldQuestNPC)], "false", "Named friendly NPC + merchant locations (quest navigation; own family, not clustered)"),
                B("quest_npc_quest_aware", questNpcQuestAware, "false", "Quest-aware quest NPCs: show a curated questline NPC's marker only while its quest is active (event flag set). Off = always shown."),
                B("show_imp_statues", showCategory[static_cast<int>(Cat::WorldImpStatues)], "false", "Imp Statue (Stonesword Key fog gate) locations"),
                B("show_paintings", showCategory[static_cast<int>(Cat::WorldPaintings)], "false", "Painting locations"),
                B("show_spirit_springs", showCategory[static_cast<int>(Cat::WorldSpiritSprings)], "false", "Spirit Spring (horse jump) locations"),
                B("show_spiritspring_hawks", showCategory[static_cast<int>(Cat::WorldSpiritspringHawks)], "false", "Spiritspring Hawk locations"),
                B("show_stakes_of_marika", showCategory[static_cast<int>(Cat::WorldStakesOfMarika)], "false", "Stakes of Marika (respawn points)"),
                B("show_summoning_pools", showCategory[static_cast<int>(Cat::WorldSummoningPools)], "false", "Summoning Pool (Martyr Effigy) locations"),
                BE("show_kindling_spirits", showCategory[static_cast<int>(Cat::WorldKindlingSpirits)], "true",
                   "ERR Kindling Spirits in Misty Forest (m60_45_37_00) - collect all 5\nbetween rests for the Kindling Spirit incantation. Markers hide\npermanently once the incantation is acquired (engine flag 1045377500),\nand individually within a run via SFX-region runtime state."),
                B("show_interactables", showCategory[static_cast<int>(Cat::WorldInteractables)], "false",
                  "Interactive world objects & puzzles. Includes:\n  - Blue seal puzzles (~65 seals across the overworld, unlock hidden cellars)\n  - \"Light flame\" interacts: Sellia chalices (3), Snow Town seal-release\n    statues (4), Siofra River lanterns (~14)\n  - Hero's Tomb direction statues (16, point at hidden Hero's Tomb caves)\nEach marker hides on activation via its own engine flag."),
                B("show_world_maps", showCategory[static_cast<int>(Cat::WorldMaps)], "false", "World Map fragment locations"),
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

            {"Compatibility",
             "Options for running alongside other mods that change item placement.",
             false, {
                B("live_loot_flags", liveLootFlags, MFG_LL_DEF,
                  "Hide loot markers using the pickup flag from the loaded regulation, so they\ndisappear correctly under the Item/Enemy Randomizer or other regulation mods."),
                B("live_loot_labels", liveLootLabels, MFG_LL_DEF,
                  "Relabel each loot marker with the item its lot currently gives, so names match\nthe randomizer. Uses more memory (copies item names into the map's name table)."),
                B("live_loot_icons", liveLootIcons, MFG_LL_DEF,
                  "Give each loot marker the icon and category of the item its lot currently\ngives, so icons and show_* toggles match the randomizer."),
                B("anonymous_loot", anonymousLoot, "false",
                  "Spoiler-free: every loot marker shows a gray \"?\" and a generic label instead\nof the real item. Overrides live_loot_labels/icons; markers still hide on pickup."),
            }},

            {"Debug",
             "Hotkey for the in-memory marker dump (developer aid). Key names: F1-F24,\nA-Z, 0-9, Space, Escape, Tab, Enter, Backspace, Home, End, PageUp, PageDown,\nInsert, Delete, arrows.",
             false, {
                B("enable_marker_dump", enableMarkerDump, "false", "Master switch for the marker dump hotkey"),
                IniEntry{"marker_dump_key", IniType::VkKey, &cfg::markerDumpKey, "F9",
                         "Key to dump decoded markers to logs/MapForGoblins_markers.log. Default: F9.", false, nullptr},
                B("debug_event_flags", debugEventFlags, "false",
                  "Observe every event flag the game sets at runtime and log each newly-seen\nflag id to logs/MapForGoblins_events.log (coverage-gap discovery aid).\nHooks SetEventFlag; off by default."),
                B("debug_item_grants", debugItemGrants, "false",
                  "Observe every inventory grant (item pickup/shop/reward) and log the raw\nrequest to logs/MapForGoblins_events.log. Hooks AddItemFunc; off by default."),
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
