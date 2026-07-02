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
    WorldLegacyDungeon,    // iconId ∈ {50,51,55,56,58,59,60,61,62,66,210,211,213,218} (per-site unique; 62 = Ashen Leyndell)
    WorldMiquellaCross,    // iconId 208 (13 rows) — DLC Miquella's Crosses (also a clean WMPP iconId)
    // ── PARITY — the remaining native WMPP pin families (2026-07-02 audit). Same live
    // build_live_landmarks pass; grouped so every pin the game still draws natively has a
    // category of ours. Deliberately skipped iconIds: 41/67 (boss pass owns them), 80 (graces),
    // 83/84/85 (structural no-text nav), 42 (legacy-dungeon sub-zone nav points), 87 (Volcano
    // Manor request markers — dynamic), 0 (ERR-custom arena rows).
    WorldChurch,           // iconId ∈ {3,20,247,248,249} (churches + cathedrals + Grand Altar)
    WorldRuins,            // iconId ∈ {5,47,250,251,252,253,254,255} (incl. underground + Finger Ruins)
    WorldRiseTower,        // iconId ∈ {8,17,68,258} (sorcerers' rises + lookout towers)
    WorldShack,            // iconId ∈ {6,259} (shacks + DLC hovels)
    WorldFort,             // iconId ∈ {18,242,243}
    WorldCastle,           // iconId ∈ {25,26,27,28,29,241} (field castles/manors, NOT legacy dungeons)
    WorldTownVillage,      // iconId ∈ {32..40,244,245,246,261}
    WorldColosseum,        // iconId 24 (3 rows)
    WorldUniqueSite,       // iconId ∈ {10,11,43,45,46,52,53,54,57,88,217,232,240,256,257,260} (one-off sites)
    // ── GROUP 2 — non-WMPP interactables (disk MSB/AEG + EMEVD) ──
    // Portal / sending gate: AEG099_510 asset bound as arg[2] of EMEVD warp template 90005605.
    // Runtime disk+EMEVD, no bake. See docs/re/windows_portal_aeg_re_findings.md.
    WorldPortal,
    // Elevator / lever-lift: a disk MSB ObjAct EVENT whose objActParamId is a "lever" ObjActParam
    // (its live ActionButtonParam prompt text is "Pull lever"/"Push lever"). Runtime disk-MSB parse,
    // no bake. See docs/re/linux_group2_prompt_binding_re_findings.md.
    WorldElevator,
    // Smithing Table: a disk MSB Asset with model AEG099_308 (3 world placements). Plain model filter
    // over the disk asset enumeration, no bake. See docs/re/linux_group2_prompt_binding_re_findings.md.
    WorldSmithingTable,
    // MFG-original: enemy drops that RESPAWN (ItemLotParam getItemFlagId==0 / repeatable) AND are a
    // notable farm target (Smithing Stones / Golden Runes / Gloveworts). Surfaces the farmable enemy
    // drops the notable-loot pass currently skips. Live, no bake. (WorldFarmableEnemy was dropped —
    // marking every respawning mob floods the map with no clean boss filter.)
    WorldFarmableCollectible,
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
