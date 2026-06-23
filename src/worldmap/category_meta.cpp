#include "category_meta.hpp"

#include "goblin_map_data.hpp" // Category enum
#include "generated_shared/goblin_overlay_icons.hpp" // ICON_CELLS (baked-cell resolve)

#include <cstring>

namespace goblin::worldmap
{
namespace
{
// Broad marker groups (by Category-name prefix) → one circle-fallback colour each.
// Atlas icons are already individually coloured by the generator, so this colour
// only shows on the handful of categories with no atlas cell (talismans, whetblades,
// utilities, throwables, world maps, quest-NPC) and on any marker whose icon fails.
enum Group : unsigned char { G_EQUIP, G_KEY, G_LOOT, G_MAGIC, G_WORLD, G_QUEST, G_REFORGED };

// Packed ImU32 ABGR (alpha 235), avoiding an ImGui include here.
constexpr unsigned abgr(int r, int g, int b)
{
    return (235u << 24) | (static_cast<unsigned>(b) << 16) | (static_cast<unsigned>(g) << 8) |
           static_cast<unsigned>(r);
}
constexpr unsigned GROUP_COLOR[] = {
    abgr(90, 150, 235),  // G_EQUIP   — blue
    abgr(235, 200, 70),  // G_KEY     — gold
    abgr(90, 230, 130),  // G_LOOT    — green
    abgr(180, 120, 235), // G_MAGIC   — purple
    abgr(80, 210, 210),  // G_WORLD   — teal
    abgr(240, 150, 60),  // G_QUEST   — orange
    abgr(235, 110, 180), // G_REFORGED— pink
};

struct CatMeta
{
    const char *key;   // atlas ICON_CELLS key, or nullptr (→ circle fallback)
    unsigned char grp; // Group, for the fallback colour
};

// One entry per Category, IN ENUM ORDER (goblin_map_data.hpp). Most keys match the
// config show_* key 1:1; the Reforged*/Kindling categories use the atlas's own names.
const CatMeta CAT[] = {
    {"show_armaments", G_EQUIP},          // EquipArmaments
    {"show_armour", G_EQUIP},             // EquipArmour
    {"show_ashes_of_war", G_EQUIP},       // EquipAshesOfWar
    {"show_spirits", G_EQUIP},            // EquipSpirits
    {nullptr, G_EQUIP},                   // EquipTalismans (no atlas icon)
    {"show_celestial_dew", G_KEY},        // KeyCelestialDew
    {"show_cookbooks", G_KEY},            // KeyCookbooks
    {"show_crystal_tears", G_KEY},        // KeyCrystalTears
    {"show_imbued_sword_keys", G_KEY},    // KeyImbuedSwordKeys
    {"show_larval_tears", G_KEY},         // KeyLarvalTears
    {"show_scadutree_fragments", G_KEY},  // KeyScadutreeFragments
    {"show_great_runes", G_KEY},          // KeyGreatRunes
    {"show_lost_ashes", G_KEY},           // KeyLostAshes
    {"show_pots_n_perfumes", G_KEY},      // KeyPotsNPerfumes
    {"show_seeds_tears", G_KEY},          // KeySeedsTears
    {nullptr, G_KEY},                     // KeyWhetblades (no atlas icon)
    {"show_ammo", G_LOOT},                // LootAmmo
    {"show_bell_bearings", G_LOOT},       // LootBellBearings
    {"show_consumables", G_LOOT},         // LootConsumables
    {"show_crafting_materials", G_LOOT},  // LootCraftingMaterials
    {"show_mp_fingers", G_LOOT},          // LootMPFingers
    {"show_material_nodes", G_LOOT},      // LootMaterialNodes
    {"show_merchant_bell_bearings", G_LOOT}, // LootMerchantBellBearings
    {"show_reusables", G_LOOT},           // LootReusables
    {"show_smithing_stones", G_LOOT},     // LootSmithingStones
    {"show_smithing_stones_low", G_LOOT}, // LootSmithingStonesLow
    {"show_smithing_stones_rare", G_LOOT},// LootSmithingStonesRare
    {"show_golden_runes", G_LOOT},        // LootGoldenRunes
    {"show_golden_runes_low", G_LOOT},    // LootGoldenRunesLow
    {"show_stonesword_keys", G_LOOT},     // LootStoneswordKeys
    {nullptr, G_LOOT},                    // LootThrowables (no atlas icon)
    {"show_prattling_pates", G_LOOT},     // LootPrattlingPates
    {"show_rune_arcs", G_LOOT},           // LootRuneArcs
    {"show_dragon_hearts", G_LOOT},       // LootDragonHearts
    {"show_gloveworts", G_LOOT},          // LootGloveworts
    {"show_great_gloveworts", G_LOOT},    // LootGreatGloveworts
    {"show_rada_fruit", G_LOOT},          // LootRadaFruit
    {"show_gestures", G_LOOT},            // LootGestures
    {"show_greases", G_LOOT},             // LootGreases
    {nullptr, G_LOOT},                    // LootUtilities (no atlas icon)
    {"show_stat_boosts", G_LOOT},         // LootStatBoosts
    {"show_fortunes", G_REFORGED},        // ReforgedFortunes
    {"show_hostile_npc", G_WORLD},        // WorldHostileNPC
    {nullptr, G_WORLD},                   // WorldQuestNPC (legacy; Quest Browser)
    {"show_incantations", G_MAGIC},       // MagicIncantations
    {"show_memory_stones", G_MAGIC},      // MagicMemoryStones
    {"show_prayerbooks", G_MAGIC},        // MagicPrayerbooks
    {"show_sorceries", G_MAGIC},          // MagicSorceries
    {"show_bosses", G_WORLD},             // WorldBosses
    {"show_deathroot", G_QUEST},          // QuestDeathroot
    {"show_progression", G_QUEST},        // QuestProgression
    {"show_seedbed_curses", G_QUEST},     // QuestSeedbedCurses
    {"show_ember_pieces", G_REFORGED},    // ReforgedEmberPieces
    {"show_items_and_changes", G_REFORGED}, // ReforgedItemsAndChanges
    {"show_rune_pieces", G_REFORGED},     // ReforgedRunePieces
    {"show_graces", G_WORLD},             // WorldGraces
    {"show_imp_statues", G_WORLD},        // WorldImpStatues
    {nullptr, G_WORLD},                   // WorldMaps (no atlas icon)
    {"show_paintings", G_WORLD},          // WorldPaintings
    {"show_spirit_springs", G_WORLD},     // WorldSpiritSprings
    {"show_spiritspring_hawks", G_WORLD}, // WorldSpiritspringHawks
    {"show_stakes_of_marika", G_WORLD},   // WorldStakesOfMarika
    {"show_summoning_pools", G_WORLD},    // WorldSummoningPools
    {"show_kindling_spirits", G_WORLD},   // WorldKindlingSpirits
    {"show_interactables", G_WORLD},      // WorldInteractables
};
constexpr int CAT_COUNT = static_cast<int>(sizeof(CAT) / sizeof(CAT[0]));
static_assert(CAT_COUNT ==
                  static_cast<int>(goblin::generated::Category::WorldInteractables) + 1,
              "category_meta CAT table out of sync with the Category enum");
} // namespace

const char *category_icon_key(int category)
{
    if (category < 0 || category >= CAT_COUNT)
        return nullptr;
    return CAT[category].key;
}

unsigned int category_color(int category)
{
    if (category < 0 || category >= CAT_COUNT)
        return GROUP_COLOR[G_LOOT];
    return GROUP_COLOR[CAT[category].grp];
}

int category_count() { return CAT_COUNT; }

bool category_has_baked_icon(int category)
{
    const char *key = category_icon_key(category);
    if (!key)
        return false;
    using namespace goblin::overlay_icons;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
            return true;
    return false;
}

namespace
{
// Canonical engine iconId per category for the baked→GPU migration. SPARSE — a category
// absent here is still baked-only ("to replace"). Populate an entry as each category's
// real engine sprite gets wired into the marker draw; the F1 completion panel then counts
// it as replaced once harvested_icon() has the sprite.
struct CategoryGpuIcon { int category; int iconId; };
constexpr CategoryGpuIcon CATEGORY_GPU_ICONS[] = {
    {-1, 0},  // sentinel (never matches a real category) — keeps the array non-empty;
              // add {static_cast<int>(Category::X), iconId} entries as categories migrate
};
} // namespace

int category_gpu_iconId(int category)
{
    for (const auto &e : CATEGORY_GPU_ICONS)
        if (e.category == category)
            return e.iconId;
    return 0;
}
} // namespace goblin::worldmap
