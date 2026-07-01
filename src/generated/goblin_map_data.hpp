#pragma once

#include "../from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

enum class Category : uint8_t
{
    EquipArmaments,
    EquipArmour,
    EquipAshesOfWar,
    EquipSpirits,
    EquipTalismans,
    KeyCelestialDew,
    KeyCookbooks,
    KeyCrystalTears,
    KeyImbuedSwordKeys,
    KeyLarvalTears,
    KeyScadutreeFragments,
    KeyGreatRunes,
    KeyLostAshes,
    KeyPotsNPerfumes,
    KeySeedsTears,
    KeyWhetblades,
    LootAmmo,
    LootBellBearings,
    LootConsumables,
    LootCraftingMaterials,
    LootMPFingers,
    LootMaterialNodes,
    LootMerchantBellBearings,
    LootReusables,
    LootSmithingStones,
    LootSmithingStonesLow,
    LootSmithingStonesRare,
    LootGoldenRunes,
    LootGoldenRunesLow,
    LootStoneswordKeys,
    LootThrowables,
    LootPrattlingPates,
    LootRuneArcs,
    LootDragonHearts,
    LootGloveworts,
    LootGreatGloveworts,
    LootRadaFruit,
    LootGestures,
    LootGreases,
    LootUtilities,
    LootStatBoosts,
    ReforgedFortunes,
    WorldHostileNPC,
    WorldQuestNPC,
    MagicIncantations,
    MagicMemoryStones,
    MagicPrayerbooks,
    MagicSorceries,
    WorldBosses,
    QuestDeathroot,
    QuestProgression,
    QuestSeedbedCurses,
    ReforgedEmberPieces,
    ReforgedItemsAndChanges,
    ReforgedRunePieces,
    WorldGraces,
    WorldImpStatues,
    WorldMaps,
    WorldPaintings,
    WorldSpiritSprings,
    WorldSpiritspringHawks,
    WorldStakesOfMarika,
    WorldSummoningPools,
    WorldKindlingSpirits,
    WorldInteractables,
    // ── MapGenie category-coverage GROUP 1 — landmarks keyed on WorldMapPointParam.iconId ──
    // Read LIVE from the active install's WorldMapPointParam (build_live_landmarks, same pattern
    // as build_live_bosses). Mod-agnostic: the iconId semantics are byte-identical vanilla↔ERR
    // (verified tools/verify_worldmap_iconids.py). No baked rows, no ERR-specific data. See
    // docs/re/windows_mapgenie_category_coverage_re_findings.md Tier 2(A).
    WorldDivineTower,      // iconId 23 (6 rows)
    WorldEvergaol,         // iconId 9
    WorldMinorErdtree,     // iconId 30 (11 rows)
    WorldGrandLift,        // iconId 21 (Dectus + Rold only — NOT in-dungeon lifts)
    WorldDungeon,          // iconId ∈ {4,13,14,15,16,230,231,234} (typed minor-dungeon union)
    WorldLegacyDungeon,    // iconId ∈ {50,51,55,56,58,59,60,61,66,210,211,213,218} (per-site unique)
    WorldMiquellaCross,    // iconId 208 (13 rows) — DLC Miquella's Crosses (also a clean WMPP iconId)
};

// Provenance of a lot-backed loot row — the bake's own classification (extract_all_items.py
// 'source' field). Lets the runtime know which baked rows the disk source may legitimately
// REPLACE (Treasure = MSB Treasure event, runtime-derivable) vs which must stay baked (Enemy
// drop on a ChrIns, Emevd scripted grant with no MSB world position). Unknown = a bake emitted
// before this field existed (the generated .cpp omits it → zero-init) → treat as legacy: keep
// the prior replace behaviour. See docs + map_entry_layer build_buckets_impl.
enum class LootSource : uint8_t
{
    Unknown = 0,
    Treasure = 1,  // MSB Treasure event placement (disk source can replace it)
    Enemy = 2,     // enemy drop (ItemLotParam_enemy / NpcParam) — no MSB Treasure, stays baked
    Emevd = 3,     // EMEVD-granted lot — no MSB world position, stays baked
};

struct MapEntry
{
    uint64_t row_id;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST data;
    Category category;
    int16_t geom_slot;    // GEOF slot = InstanceID - 9000; -1 if N/A
    int16_t name_suffix;  // e.g. 9003 from "AEG099_821_9003"; -1 if N/A
    const char *object_name;  // full MSB object name e.g. "AEG099_821_9003"; nullptr if N/A
    // Source item-lot linkage for live-loot mode (read getItemFlagId/item from
    // memory at runtime → randomizer-compatible). 0/0 = not lot-backed.
    uint32_t lotId;       // ItemLotParam row id (0 = none)
    uint8_t  lotType;     // 0 = none, 1 = ItemLotParam_map (pref), 2 = ItemLotParam_enemy (pref)
    // Provenance (see LootSource). Trailing field: a pre-provenance generated .cpp omits it,
    // so it zero-inits to Unknown and the runtime keeps its legacy behaviour. 0 = Unknown.
    LootSource loot_source;
};

extern const MapEntry MAP_ENTRIES[];
extern const size_t MAP_ENTRY_COUNT;

} // namespace goblin::generated
