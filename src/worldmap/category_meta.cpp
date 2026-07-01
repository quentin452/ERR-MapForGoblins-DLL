#include "category_meta.hpp"
#include "../goblin_overlay_render_api.hpp"

#include "goblin_map_data.hpp" // Category enum
#include "generated_shared/goblin_overlay_icons.hpp" // ICON_CELLS (baked-cell resolve)

#include <atomic>
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
    {"show_quest_npc", G_WORLD},          // WorldQuestNPC (QuestNpcLayer; no atlas art yet -> circle fallback)
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
    // Summoning Pool → Martyr Effigy glyph (MENU_MAP_89, SB_MapCursor_02). Resolved by iconId via
    // map_point_rect(89) → disk fallback in MapPointProvider, so it is mod-agnostic (not ERR-baked).
    {static_cast<int>(goblin::generated::Category::WorldSummoningPools), 89},
    // Quest NPC → framed-hood NPC glyph (MENU_MAP_80, SB_MapCursor_02, rect 554,364,124,124 — the
    // vanilla quest-NPC map symbol, eye-confirmed in docs/memory/features/map-point-glyph-ids.md).
    // iconId path → map_point_rect(80) → disk fallback in MapPointProvider, so it is mod-agnostic
    // (reads the ACTIVE install's SB_MapCursor, not the ERR-baked atlas). Falls to circle when the
    // glyph can't be resolved. NB: the SB_MapCursor NN (80), NOT the WorldMapPointParam iconId
    // 443=questNPC — 443 is never resident (vanilla draws no item pins) so only the NN path works.
    {static_cast<int>(goblin::generated::Category::WorldQuestNPC), 80},
};
} // namespace

int category_gpu_iconId(int category)
{
    for (const auto &e : CATEGORY_GPU_ICONS)
        if (e.category == category)
            return e.iconId;
    return 0;
}

namespace
{
// NAME-keyed engine map symbol per category (ERR custom MENU_MAP_ERR_* / vanilla MENU_MAP_*).
// SPARSE — most categories have no real game symbol and stay on the baked atlas. Resolved via
// goblin::map_icon_rect_by_name → overlay::native_map_point_icon_by_name. Grow as symbols are
// confirmed resident in-game (see the [MAPICON-AVAIL] log + g_map_icon_named).
struct CategoryGpuName { int category; const char *name; float scale; };
using C = goblin::generated::Category;
constexpr CategoryGpuName CATEGORY_GPU_NAMES[] = {
    {static_cast<int>(C::WorldBosses), "MENU_MAP_ERR_Boss", 1.0f},
    // Normal hostile entities reuse the boss symbol, drawn smaller so a real boss still reads as the
    // bigger pin. Same symbol → the central pump already keeps it resident (no extra want).
    {static_cast<int>(C::WorldHostileNPC), "MENU_MAP_ERR_Boss", 0.65f},
};
} // namespace

const char *category_gpu_icon_name(int category)
{
    for (const auto &e : CATEGORY_GPU_NAMES)
        if (e.category == category)
            return e.name;
    return nullptr;
}

float category_gpu_icon_scale(int category)
{
    for (const auto &e : CATEGORY_GPU_NAMES)
        if (e.category == category)
            return e.scale;
    return 1.0f;
}

namespace
{
// Representative item iconId per category, in enum order. -1/0 = none. std::atomic so the map-build
// thread (writer) and the render/panel/load threads (readers) don't tear on the int.
std::atomic<int> g_rep_icon[CAT_COUNT];
} // namespace

void set_category_rep_icon(int category, int iconId)
{
    if (category < 0 || category >= CAT_COUNT)
        return;
    g_rep_icon[category].store(iconId > 0 ? iconId : 0, std::memory_order_relaxed);
}

int category_rep_icon(int category)
{
    if (category < 0 || category >= CAT_COUNT)
        return 0;
    return g_rep_icon[category].load(std::memory_order_relaxed);
}

int category_rep_icons(int (&out)[128])
{
    int n = 0;
    for (int c = 0; c < CAT_COUNT && n < 128; ++c)
    {
        int id = g_rep_icon[c].load(std::memory_order_relaxed);
        if (id > 0)
            out[n++] = id;
    }
    return n;
}

bool category_is_gpu_native(int category)
{
    // Mirror map_renderer's IconSet::resolve order + GraceLayer's dedicated grace draw. Any of
    // these means the category renders as a real engine sprite, not the baked atlas cell.
    if (category_gpu_icon_name(category) != nullptr)  // name-keyed symbol (e.g. bosses)
        return true;
    if (category_gpu_iconId(category) > 0)            // numeric MENU_MAP_<NN> map-point symbol
        return true;
    if (category_rep_icon(category) > 0)              // representative item-icon (00_Solo atlas)
        return true;
    if (category == static_cast<int>(goblin::generated::Category::WorldGraces))
        return true;                                  // graces: GraceLayer s_grace_tex / dungeon sprite
    return false;
}
} // namespace goblin::worldmap
