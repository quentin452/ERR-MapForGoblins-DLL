#include "category_meta.hpp"

#include "goblin_map_data.hpp" // Category enum

namespace goblin::worldmap
{
namespace
{
// One entry per Category, IN ENUM ORDER (goblin_map_data.hpp). The string is the
// atlas ICON_CELLS key; nullptr where the atlas has no matching icon → circle
// fallback. Most match the config show_* key 1:1; the Reforged*/Kindling categories
// use the atlas's own names (show_ember_pieces / show_items_and_changes /
// show_rune_pieces / show_fortunes / show_kindling_spirits).
const char *const ICON_KEY[] = {
    "show_armaments",          // EquipArmaments
    "show_armour",             // EquipArmour
    "show_ashes_of_war",       // EquipAshesOfWar
    "show_spirits",            // EquipSpirits
    nullptr,                   // EquipTalismans (no atlas icon)
    "show_celestial_dew",      // KeyCelestialDew
    "show_cookbooks",          // KeyCookbooks
    "show_crystal_tears",      // KeyCrystalTears
    "show_imbued_sword_keys",  // KeyImbuedSwordKeys
    "show_larval_tears",       // KeyLarvalTears
    "show_scadutree_fragments",// KeyScadutreeFragments
    "show_great_runes",        // KeyGreatRunes
    "show_lost_ashes",         // KeyLostAshes
    "show_pots_n_perfumes",    // KeyPotsNPerfumes
    "show_seeds_tears",        // KeySeedsTears
    nullptr,                   // KeyWhetblades (no atlas icon)
    "show_ammo",               // LootAmmo
    "show_bell_bearings",      // LootBellBearings
    "show_consumables",        // LootConsumables
    "show_crafting_materials", // LootCraftingMaterials
    "show_mp_fingers",         // LootMPFingers
    "show_material_nodes",     // LootMaterialNodes
    "show_merchant_bell_bearings", // LootMerchantBellBearings
    "show_reusables",          // LootReusables
    "show_smithing_stones",    // LootSmithingStones
    "show_smithing_stones_low",// LootSmithingStonesLow
    "show_smithing_stones_rare",// LootSmithingStonesRare
    "show_golden_runes",       // LootGoldenRunes
    "show_golden_runes_low",   // LootGoldenRunesLow
    "show_stonesword_keys",    // LootStoneswordKeys
    nullptr,                   // LootThrowables (no atlas icon)
    "show_prattling_pates",    // LootPrattlingPates
    "show_rune_arcs",          // LootRuneArcs
    "show_dragon_hearts",      // LootDragonHearts
    "show_gloveworts",         // LootGloveworts
    "show_great_gloveworts",   // LootGreatGloveworts
    "show_rada_fruit",         // LootRadaFruit
    "show_gestures",           // LootGestures
    "show_greases",            // LootGreases
    nullptr,                   // LootUtilities (no atlas icon)
    "show_stat_boosts",        // LootStatBoosts
    "show_fortunes",           // ReforgedFortunes
    "show_hostile_npc",        // WorldHostileNPC
    nullptr,                   // WorldQuestNPC (legacy; Quest Browser handles it)
    "show_incantations",       // MagicIncantations
    "show_memory_stones",      // MagicMemoryStones
    "show_prayerbooks",        // MagicPrayerbooks
    "show_sorceries",          // MagicSorceries
    "show_bosses",             // WorldBosses
    "show_deathroot",          // QuestDeathroot
    "show_progression",        // QuestProgression
    "show_seedbed_curses",     // QuestSeedbedCurses
    "show_ember_pieces",       // ReforgedEmberPieces
    "show_items_and_changes",  // ReforgedItemsAndChanges
    "show_rune_pieces",        // ReforgedRunePieces
    "show_graces",             // WorldGraces
    "show_imp_statues",        // WorldImpStatues
    nullptr,                   // WorldMaps (no atlas icon)
    "show_paintings",          // WorldPaintings
    "show_spirit_springs",     // WorldSpiritSprings
    "show_spiritspring_hawks", // WorldSpiritspringHawks
    "show_stakes_of_marika",   // WorldStakesOfMarika
    "show_summoning_pools",    // WorldSummoningPools
    "show_kindling_spirits",   // WorldKindlingSpirits
    "show_interactables",      // WorldInteractables
};
constexpr int ICON_KEY_COUNT = static_cast<int>(sizeof(ICON_KEY) / sizeof(ICON_KEY[0]));
// Compile-time guard: the table must cover every Category, in order.
static_assert(ICON_KEY_COUNT ==
                  static_cast<int>(goblin::generated::Category::WorldInteractables) + 1,
              "category_meta ICON_KEY out of sync with the Category enum");
} // namespace

const char *category_icon_key(int category)
{
    if (category < 0 || category >= ICON_KEY_COUNT)
        return nullptr;
    return ICON_KEY[category];
}
} // namespace goblin::worldmap
