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
    bool collectedGraying = true;
    bool hideCollected = false;
    bool showRegionLabels = true;  // overlay: draw major-region name labels on the map
    bool diagLootFlags = false;    // one-shot [LOOTDIAG] field dump for the collected-flag RE
    bool debugLogging = false;
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

    bool redifyBossIcons = false;

    bool enableMarkerDump = false;
    uint32_t markerDumpKey = 0x78; // VK_F9
    bool debugEventFlags = false;
    bool debugItemGrants = false;
    bool debugFlagCapture = false;
    bool debugWorldmapProbe = false;
    bool dumpIconTextures = false;
    bool liveProjection = true;
    bool dumpConverters = false;
    bool dumpNativePins = false;
    bool overlayMarkersProto = false;
    bool debugRenderDims = false;
    bool fixMidsessionResolution = false;
    bool nativeMapInjection = false;
    bool liveRefreshWorldMap = false;
    float overlayMasterScale = 1.0f;   // master scale for all overlay markers + piles
    float overlayIconScale = 1.0f;     // category marker icons (× master)
    float overlayClusterScale = 1.0f;  // cluster pile glyphs (× master)

    // In-game minimap HUD (corner, north-up, overworld-only — underground player pos
    // is not yet reliable). Foundation/opt-in; off by default.
    bool debugClusterAnchors = false; // viz: pile anchor + member lines + name + d/thr
    bool debugRegionVolumes = false; // viz: draw each MapNameOverride volume + name
    bool showMinimap = false;
    float minimapZoom = 0.08f;     // px per world-unit shown on the minimap
    float minimapSize = 130.0f;    // minimap radius in px
    float minimapOpacity = 0.85f;  // background disc opacity 0..1
    bool minimapAnchorRight = true;   // corner: right vs left
    bool minimapAnchorBottom = false; // corner: bottom vs top
    float minimapOffsetX = 0.0f;      // px offset from the anchored corner
    float minimapOffsetY = 0.0f;

    // In-game per-section visibility (the 7 display groups). Persisted so an
    // in-game toggle survives relaunch. Default all-visible = no behaviour change.
    bool sectionEquipment = true, sectionKeyItems = true, sectionLoot = true,
         sectionMagic = true, sectionQuest = true, sectionReforged = true,
         sectionWorld = true;

    // Marker clustering (v1, density-triggered, static). Collapses dense marker
    // piles into one cluster icon to cut the per-page map-open cost. Opt-in.
    bool enableClustering = false;
    bool clusterHard = false;         // hard = mix categories into one pile; soft = per-category
    uint8_t clusterThreshold = 4;     // a location clusters only if it holds > this many markers; the FAR (clustered) size when distance-adaptive
    std::string clusterExclude = "";  // category names kept exact (never clustered)
    std::string clusterThresholdOverrides = "";  // "Name:N" per-category threshold overrides
    // Distance-adaptive clustering: NEAR the player use a HIGH threshold (few piles
    // → detail / real items), ramping DOWN to clusterThreshold FAR away (more
    // clustering → fewer distant icons). Linear ramp over nearRadius..farRadius
    // tiles. Applied per pile at map-open replan.
    bool    clusterDistanceAdaptive = false;
    uint8_t clusterNearThreshold = 60; // detail size NEAR player (high = more individual items)
    uint8_t clusterNearRadius    = 1;  // tiles: full-detail radius (tight = just your immediate area)
    uint8_t clusterFarRadius     = 2;  // tiles: at/beyond this, clusterThreshold (clustered)
    bool clusterDebugRadius = false;  // overlay: draw distance-adaptive zones (player + near/far rings + tabs)
    bool questNpcQuestAware = false;  // gate quest-NPC markers on quest-active flags
    std::string questProgress = "";   // Quest Browser per-step done bits ('0'/'1')
    bool questGreyOnDeath = true;     // grey questlines whose NPC death flag is set
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
                         "MINIMUM seconds to wait before loading map icons. The mod now POLLS\n"
                         "for the world-map data to finish loading (robust on slow PCs) and\n"
                         "proceeds as soon as it's ready; this is just a floor/settle margin.\n"
                         "Leave at 5 unless you want a longer guaranteed settle.", false, nullptr},
                B("require_map_fragments", requireMapFragments, "true",
                  "Require map fragment discovery before showing icons in that area\n"
                  "(overlay map: gates on the game's real fog-of-war reveal state)."),
                B("collected_graying", collectedGraying, "true",
                  "Overlay map: dim+desaturate markers for items already collected and\n"
                  "bosses already cleared (cleared bosses also get a green checkmark).\n"
                  "FALSE = always draw markers at full brightness."),
                B("hide_collected", hideCollected, "false",
                  "Overlay map: when collected_graying is on, HIDE collected/cleared\n"
                  "markers entirely instead of dimming them (legacy native-map behaviour)."),
                B("show_region_labels", showRegionLabels, "true",
                  "Overlay map: draw the major-region names (Limgrave, Caelid, Liurnia,\n"
                  "Altus Plateau, ...) on the open map page, beneath the markers."),
                B("diag_loot_flags", diagLootFlags, "false",
                  "RE diagnostic: on map build, log [LOOTDIAG] for a few loot markers per\n"
                  "category — every candidate pickup flag (lot-wide, 8 per-slot, baked) and\n"
                  "whether each reads SET. Run on a 100% save to find the real collected\n"
                  "flag for the categories the census over-reports. Off by default."),
                B("debug_logging", debugLogging, "false",
                  "Enable verbose debug logging (memory addresses, param details, FMG internals)"),
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
                IniEntry{"quest_progress", IniType::String, &cfg::questProgress, "",
                         "Quest Browser per-step checkmarks (one 0/1 per step, author order).\n"
                         "Managed by the overlay's Quest Browser; saved when you Save.",
                         false, nullptr},
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

            {"Quest Browser",
             "Settings for the in-overlay Quest Browser (F1).",
             false, {
                B("grey_unfinishable_on_death", questGreyOnDeath, "true",
                  "Grey out a questline and tag it [unfinishable] (or [concluded] for\n"
                  "merchant-type NPCs) when the overlay reads that NPC's death/conclusion\n"
                  "event flag as set.\n"
                  "WARNING: EXPERIMENTAL / potentially buggy. The per-NPC death flags are\n"
                  "reverse-engineered from EMEVD/TalkESD; some may be wrong, shared with\n"
                  "normal quest completion, or simply not set in your particular save -- so\n"
                  "a questline can be greyed when it shouldn't be, or not greyed when its\n"
                  "NPC is actually gone. Set to false to always show every questline\n"
                  "normally. The same toggle is in the Quest Browser itself."),
            }},

            {"Clustering",
             "Collapses dense piles of markers into a single cluster icon, so the world\n"
             "map opens fast even with everything enabled (the open cost scales with how\n"
             "many markers are on the displayed page). Density-triggered: only spots with\n"
             "more than the threshold of markers cluster; sparse markers stay exact.",
             false, {
                B("enable_clustering", enableClustering, "false",
                  "Master switch for marker clustering."),
                B("cluster_hard", clusterHard, "false",
                  "HARD clustering: fold ALL marker types in a dense map cell into ONE\n"
                  "mixed pile (far fewer icons). false = SOFT: cluster each category\n"
                  "separately (typed piles, e.g. 'Smithing Stones (12)'). Excluded\n"
                  "categories stay exact in both. Save + restart (re-plans the piles)."),
                IniEntry{"cluster_threshold", IniType::U8, &cfg::clusterThreshold, "4",
                         "A location clusters only if it holds MORE than this many markers.\n"
                         "LOWER = MORE aggressive (groups smaller piles). NOT higher.\n"
                         "When distance-adaptive is on, this is the size NEAR the player.", false, nullptr},
                B("cluster_distance_adaptive", clusterDistanceAdaptive, "false",
                  "Scale cluster size by distance from the player: full detail near you,\n"
                  "distant dense spots merge harder (fewer far icons). Linear ramp from\n"
                  "cluster_near_threshold (near, high) DOWN to cluster_threshold (far)."),
                IniEntry{"cluster_near_threshold", IniType::U8, &cfg::clusterNearThreshold, "60",
                         "Distance-adaptive: detail cluster size NEAR the player (high = more\n"
                         "individual items shown near you). Ramps down to cluster_threshold far.", false, nullptr},
                IniEntry{"cluster_near_radius", IniType::U8, &cfg::clusterNearRadius, "1",
                         "Distance-adaptive: full-detail radius around the player, in 256-unit tiles.", false, nullptr},
                IniEntry{"cluster_far_radius", IniType::U8, &cfg::clusterFarRadius, "2",
                         "Distance-adaptive: at/beyond this many tiles, use cluster_threshold (full clustering).", false, nullptr},
                B("cluster_debug_radius", clusterDebugRadius, "false",
                  "Overlay DEBUG: draw the distance-adaptive zones — player marker, the\n"
                  "near/far radius rings (overworld), and each pile's sub-page tab\n"
                  "(underground) — to see where the ramp engages. Off by default."),
                IniEntry{"cluster_exclude", IniType::String, &cfg::clusterExclude, "",
                         "Categories that stay EXACT markers and never fold into a cluster\n"
                         "(comma-separated, matched loosely vs the category name, e.g.\n"
                         "Graces, Bosses, GreatRunes). Driven by the per-category 'cluster'\n"
                         "checkboxes in the overlay (F1); takes effect after Save + restart.",
                         false, nullptr},
                IniEntry{"cluster_threshold_overrides", IniType::String, &cfg::clusterThresholdOverrides, "",
                         "Per-category cluster thresholds: \"Name:N\" comma-separated, by\n"
                         "EXACT category name (e.g. SmithingStones:4,WorldBosses:20). A\n"
                         "category not listed uses cluster_threshold above. Each category\n"
                         "now clusters separately per map cell. Driven by the overlay's\n"
                         "per-category threshold inputs; takes effect after Save + restart.",
                         false, nullptr},
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
                B("show_quest_npc", showCategory[static_cast<int>(Cat::WorldQuestNPC)], "false", "LEGACY / UNFINISHED (superseded by the in-overlay Quest Browser): raw quest-NPC + merchant map pins. Off by default."),
                B("quest_npc_quest_aware", questNpcQuestAware, "false", "LEGACY / UNFINISHED (superseded by the Quest Browser): gate the legacy quest-NPC map pins on their questline flag (show only while the quest is active). Needs show_quest_npc. Off by default."),
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
             "Display options for boss markers on the overlay map.",
             ERR, {
                BE("redify_boss_icons", redifyBossIcons, "false",
                   "Cosmetic: draw boss markers red and auto-hide them once the boss is\ndefeated."),
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
                B("debug_flag_capture", debugFlagCapture, "false",
                  "Quest Browser death-flag capture tool (overlay Dev tools): arm naming an\nNPC, kill it, finalize -> the persisted flag(s) are written to\nlogs/MapForGoblins_flagcapture.txt as NpcQuest::fail_flag candidates.\nInstalls the SetEventFlag hook in a LIGHT mode (no coverage drain); off by default."),
                B("debug_worldmap_probe", debugWorldmapProbe, "false",
                  "Dev probe: log the world-map cursor coords (read-only) + the live view\nprojection (pan/zoom @ WorldMapArea+0x378/+0x380, virtual canvas) to confirm\nthe world->screen transform. Open the world map, move the cursor, then PAN\nand ZOOM. Logs to logs/MapForGoblins_wmprobe.log; off by default."),
                B("dump_icon_textures", dumpIconTextures, "false",
                  "Dev probe: hook the GFx image creator and log each worldmap icon image\n(sprite rect + backing GPU texture id) as [ICONTEX] when the map opens, to\nmap iconIds to their sprite-sheet sub-rects for runtime icon textures. Off by\ndefault."),
                B("live_projection", liveProjection, "true",
                  "Project markers with the engine's OWN world->map-space function (the\nlive WorldMapViewModel) instead of our baked LEGACY_CONV + affine + DLC\neyeball. Fixes hundreds of dungeon/underground misplacements via the game's\nreal LegacyConv fold. Falls back to the baked projection when the map is\nclosed or an area isn't placed by the game. On by default; set false to\nforce the old baked projection."),
                B("dump_converters", dumpConverters, "false",
                  "Dev one-shot RE check: when the world map is open, find the live\nCS::WorldMapViewModel and dump its 8 converter slots (origin/bias/scale/\narea/legacyConvNode @ VM+0xF8) + count to MapForGoblins.log as [CONV]. Open\nthe overworld, then base underground (m12), then the DLC/Realm of Shadows map\nto capture each page's converter (incl. the never-solved DLC constants).\nConfirms the world->map-space projection RE. Off by default."),
                B("dump_native_pins", dumpNativePins, "false",
                  "Dev one-shot RE check: when the world map is open, walk the native-pin\nicon manager (CSWorldMapPointMan std::map @ mgr+0x398, mgr=[er+0x3D6E9B0])\nand its sibling [er+0x3D6F558], dumping each built pin's key/ins-ptr + a\nfield window as [PINS] to MapForGoblins.log. Identifies WHAT native pins\nstill draw (graces/categories/objectives) + their source, to decide native-\npin suppression (overlay = sole icon source). Read-only. Off by default."),
                B("overlay_markers_proto", overlayMarkersProto, "false",
                  "Dev prototype (overlay-rendered markers): draw our own marker dot in\nthe ImGui overlay, projected onto the open world map via the live pan/zoom\n(WorldMapArea), to verify the world->screen affine. Starts the cursor probe\nif not already on. Open the F1 menu to tune scale/bias live. Off by default."),
                B("fix_midsession_resolution", fixMidsessionResolution, "false",
                  "EXPERIMENTAL no-restart fix for the mid-session resolution-change zoom\n(3D world + map stay zoomed after changing resolution in-game). On a swapchain\nresize, raw-pokes ER's render-output dims to the new size (the engine leaves\nthem stale). Same-aspect changes only (16:9<->16:9, no letterbox). Off by\ndefault; enable + test. If the zoom persists or anything misbehaves, set false\n(restarting ER after a resolution change is the safe fallback)."),
                B("debug_render_dims", debugRenderDims, "false",
                  "Dev diagnostic (mid-session resolution bug): every ~2s log ER's render-\noutput dims (active +0x118/+0x11c vs the live backbuffer) + dirty bits to\nMapForGoblins.log as [RENDIMS]. Change the resolution in-game, then read the\nlog: the entry that stays at the OLD resolution is the stale one. Off by default."),
                IniEntry{"overlay_master_scale", IniType::F32, &cfg::overlayMasterScale, "1.0",
                  "Master scale for ALL overlay map markers + cluster piles (multiplies the\nper-type scales). 1.0 = default. A resolution-relative base size is applied\nfirst, then ×master×type. Editable live in the F1 menu; persists on Save."},
                IniEntry{"overlay_icon_scale", IniType::F32, &cfg::overlayIconScale, "1.0",
                  "Scale for category marker ICONS (x master). 1.0 = default."},
                IniEntry{"overlay_cluster_scale", IniType::F32, &cfg::overlayClusterScale, "1.0",
                  "Scale for CLUSTER pile glyphs (x master). 1.0 = default."},
                B("debug_cluster_anchors", debugClusterAnchors, "false",
                  "Debug viz: per cluster pile, draw the anchor + lines to every member + the\nname + distance/threshold. Green = grace anchor, red CENTROID = anchor missing.\nSeparate from the distance rings (cluster_debug_radius). Off by default."),
                B("debug_region_volumes", debugRegionVolumes, "false",
                  "Debug viz: draw every MapNameOverride region volume on the open map page at\nits projected centre + its name; RED = the textId does NOT resolve in the FMG\n(the bug), cyan = resolves. Off by default."),
                B("show_minimap", showMinimap, "false",
                  "In-game minimap HUD: a small north-up minimap in a screen corner showing\nnearby goblin markers around the player during gameplay (not the pause-screen\nmap). OVERWORLD only for now (underground player position isn't reliable yet).\nFoundation/opt-in; off by default."),
                IniEntry{"minimap_zoom", IniType::F32, &cfg::minimapZoom, "0.08",
                  "Minimap zoom = pixels per world-unit. Higher = more zoomed-in (less area\nshown). 0.08 = default."},
                IniEntry{"minimap_size", IniType::F32, &cfg::minimapSize, "130",
                  "Minimap radius in pixels. 130 = default."},
                IniEntry{"minimap_opacity", IniType::F32, &cfg::minimapOpacity, "0.85",
                  "Minimap background disc opacity, 0..1. 0.85 = default."},
                B("minimap_anchor_right", minimapAnchorRight, "true",
                  "Minimap corner: anchor to the RIGHT edge (false = left). Default true."),
                B("minimap_anchor_bottom", minimapAnchorBottom, "false",
                  "Minimap corner: anchor to the BOTTOM edge (false = top). Default false."),
                IniEntry{"minimap_offset_x", IniType::F32, &cfg::minimapOffsetX, "0",
                  "Minimap X offset (px) from the anchored corner. 0 = default."},
                IniEntry{"minimap_offset_y", IniType::F32, &cfg::minimapOffsetY, "0",
                  "Minimap Y offset (px) from the anchored corner. 0 = default."},
                B("native_map_injection", nativeMapInjection, "false",
                  "Inject goblin markers into the game's native world map\n(WorldMapPointParam). FALSE (default) = the ImGui overlay is the SOLE map\nsource (no native page-build, so no map-open freeze and no double-draw) — all\nfeatures are ported to the overlay. TRUE = also inject the legacy native icons\n(the old behaviour); kept only as a fallback and slated for removal."),
                B("live_refresh_world_map", liveRefreshWorldMap, "false",
                  "EXPERIMENTAL: re-render world-map icons WHILE the map is open when you\ntoggle a section/category (instead of only on the next map open). Hooks the\ngame's own placed-map-point (re)build and replays it with the engine's real\ncontext on its own thread. Off by default -- enable to test; if icons don't\nupdate live or anything misbehaves, set back to false (toggles still apply on\nthe next map open). See docs/windows_re_live_refresh_capture.md."),
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
