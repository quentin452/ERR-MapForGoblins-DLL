#include "goblin_logic.hpp"
#include "goblin_config.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "goblin/goblin_structs.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "goblin/goblin_map_tiles.hpp"
#include "goblin/goblin_map_exceptions.hpp"
#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_bench.hpp"

#include <spdlog/spdlog.h>

using namespace goblin;
using namespace goblin::mapPoint;

// Goblin icon ID ranges (same as Goblin-ERR)
static constexpr ParamRange goblinIcons(1, 78500);
static constexpr ParamRange goblinIconsERR(1000000, 10025000);

static bool HasException(int paramId, int &mapFragment)
{
    if (ExceptionList.count(paramId))
    {
        mapFragment = ExceptionList.at(paramId);
        return true;
    }
    return false;
}

static int GetMapFlagFromTile(MapTile location)
{
    for (const auto &fragment : MapList)
    {
        for (auto &chunk : fragment.mapFragmentTile)
        {
            if (chunk == location)
                return fragment.mapFragmentId;
        }
    }
    return 0;
}

// Region name for a cluster tile, via the map-fragment grouping (the same tile→
// fragment map used for fragment-eviction). Coarse (~26 regions) — enough to label
// a cluster "Leyndell (507)" instead of a bare count. "" if the tile maps nowhere.
std::string goblin::cluster_region_label(int area, int gx, int gz)
{
    namespace f = goblin::flag;
    switch (GetMapFlagFromTile(MapTile(area, gx, gz)))
    {
    case f::FarumAzula:        return "Crumbling Farum Azula";
    case f::Haligtree:         return "Haligtree";
    case f::WestLimgrave:      return "West Limgrave";
    case f::WeepingPeninsula:  return "Weeping Peninsula";
    case f::EastLimgrave:      return "Limgrave";
    case f::EastLiurnia:       return "East Liurnia";
    case f::NorthLiurnia:      return "North Liurnia";
    case f::WestLiurnia:       return "West Liurnia";
    case f::Altus:             return "Altus Plateau";
    case f::Leyndell:          return "Leyndell";
    case f::Gelmir:            return "Mt. Gelmir";
    case f::Caelid:            return "Caelid";
    case f::Dragonbarrow:      return "Dragonbarrow";
    case f::MountaintopsWest:  return "Mountaintops of the Giants";
    case f::MountaintopsEast:  return "Mountaintops of the Giants";
    case f::Snowfields:        return "Consecrated Snowfields";
    case f::Ainsel:            return "Ainsel River";
    case f::LakeOfRot:         return "Lake of Rot";
    case f::Mohgwyn:           return "Mohgwyn Palace";
    case f::Siofra:            return "Siofra River";
    case f::Deeproot:          return "Deeproot Depths";
    case f::GravesitePlain:    return "Gravesite Plain";
    case f::ScaduAltus:        return "Scadu Altus";
    case f::SouthernShore:     return "Cerulean Coast";
    case f::RauhRuins:         return "Rauh Base";
    case f::Abyss:             return "Abyssal Woods";
    default:                   return "";
    }
}

// Fallback name by ORIGINAL area (the big areas a projected cluster comes from).
// Conservative: only areas we're confident about — a wrong name is worse than a
// bare count, so unknown → "".
std::string goblin::area_region_label(int area)
{
    switch (area)
    {
    case 10: return "Stormveil Castle";
    case 11: return "Leyndell, Royal Capital";
    case 12: return "Underground";              // Siofra/Ainsel/Nokron/Deeproot (Eternal Cities)
    case 15: return "Haligtree";                // Elphael / Miquella's Haligtree
    case 35: return "Leyndell, Ashen Capital";
    default: return "";
    }
}

static void SetSecondaryFlags(from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, int flagId)
{
    row.textEnableFlag2Id1 = flagId;
    row.textEnableFlag2Id2 = flagId;
    row.textEnableFlag2Id3 = flagId;
    row.textEnableFlag2Id4 = flagId;
    row.textEnableFlag2Id5 = flagId;
    row.textEnableFlag2Id6 = flagId;
    row.textEnableFlag2Id7 = flagId;
    row.textEnableFlag2Id8 = flagId;
}

static int GetMapFragment(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    int requiredMapFragment = 0;
    auto chunk = MapTile(row.areaNo, row.gridXNo, row.gridZNo);

    if (!HasException(rowId, requiredMapFragment))
    {
        requiredMapFragment = GetMapFlagFromTile(chunk);
    }

    if (config::requireMapFragments)
    {
        // Post-event areas
        if (chunk == MapTile(11, 5) || chunk == MapTile(19))
        {
            SetSecondaryFlags(row, flag::StoryErdtreeOnFire);
        }
        else if (chunk == MapTile(21) || chunk == MapTile(21, 1) ||
                 chunk == MapTile(21, 2) || chunk == MapTile(22))
        {
            SetSecondaryFlags(row, flag::StoryCharmBroken);
        }
        else if (chunk == MapTile(20, 1))
        {
            SetSecondaryFlags(row, flag::StorySealingTreeBurnt);
        }
    }

    return requiredMapFragment;
}

static int GetIconFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (config::requireMapFragments)
        return GetMapFragment(rowId, row);
    else
        return flag::AlwaysOn;
}

static void HideOnCompletion(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (row.textId2 == 5100)
    {
        auto disableFlag = row.textEnableFlagId4;
        row.textDisableFlagId1 = disableFlag;
        row.textDisableFlagId2 = disableFlag;
        row.textDisableFlagId3 = disableFlag;
        row.textDisableFlagId4 = disableFlag;
    }
    else
    {
        auto disableFlag = row.textEnableFlagId5;
        row.textDisableFlagId1 = disableFlag;
        row.textDisableFlagId2 = disableFlag;
        row.textDisableFlagId3 = disableFlag;
        row.textDisableFlagId4 = disableFlag;
        row.textDisableFlagId5 = disableFlag;
    }
}

// In ERR's custom icon tga, iconId 372/373 regions are transparent — assigning
// them makes the marker invisible. iconId 374 is the valid "red skull / boss"
// region. The rest of the generator already uses 374 for boss markers.
static constexpr int kRedSkullIconId = 374;

static void SetupOverworldERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    row.textEnableFlagId2 = row.eventFlagId;
    row.eventFlagId = GetIconFlag(rowId, row);

    if (config::redifyBossIcons)
    {
        row.iconId = kRedSkullIconId;
        HideOnCompletion(rowId, row);
    }
}

static void SetupDungeonERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    int mapFragment = GetIconFlag(rowId, row);
    row.textEnableFlagId1 = mapFragment;
    row.textEnableFlagId2 = mapFragment;
    row.textEnableFlagId3 = row.eventFlagId;
    row.eventFlagId = 0;

    if (config::redifyDungeonIcons)
    {
        row.iconId = kRedSkullIconId;
    }
    if (config::hideDungeonIconsOnClear)
    {
        HideOnCompletion(rowId, row);
    }
}

static void SetupCampsERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    row.textEnableFlagId2 = row.eventFlagId;
    row.eventFlagId = GetIconFlag(rowId, row);
}

static void SetupMerchants(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (config::requireMapFragments)
        row.textEnableFlagId3 = GetIconFlag(rowId, row);
    else
        row.textEnableFlagId3 = flag::AlwaysOn;
}

void goblin::apply_map_logic()
{
    GOBLIN_BENCH("map.apply_logic.total");
    spdlog::debug("Applying map fragment logic...");

    int modified_goblin = 0;
    int modified_boss = 0;
    int modified_camp = 0;
    int modified_merchant = 0;
    int scanned_rows = 0;

    for (auto [rowId, row] :
         from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam"))
    {
        scanned_rows++;
        // Goblin icons (our injected entries + any existing ones)
        if (goblinIcons.IsInRange(rowId) || goblinIconsERR.IsInRange(rowId))
        {
            // Leyndell Ashen Capital markers: gate on the Erdtree-burn flag so
            // they appear only in the ashen state (burn implies Leyndell is
            // already discovered, so we drop the map-fragment gate for them).
            if (goblin::is_ashen_capital_row(rowId))
                row.eventFlagId = flag::StoryErdtreeOnFire;
            else
                row.eventFlagId = GetIconFlag(rowId, row);

            // Fragment-eviction: a row gated on an event flag (map fragment /
            // story flag) is invisible until that flag turns on, yet still costs
            // on every map open. Register it so the refresh loop parks it off-page
            // (areaNo=99) while undiscovered and restores it the moment the flag
            // sets. Skip rows whose areaNo is already owned by collected/kindling.
            // (Verified correct on a zero-fragment save: all gate flags read false
            // → all parked, matching require_map_fragments=true semantics.)
            if (row.eventFlagId != flag::AlwaysOn && row.eventFlagId != 0 &&
                !goblin::collected::is_registered(rowId) &&
                !goblin::kindling::is_registered(rowId))
            {
                goblin::register_fragment_gated_row(&row, row.areaNo, row.eventFlagId);
            }
            modified_goblin++;
        }
        // Camp markers (textId2=5000) — ERR-placed, opt-in patching
        else if (row.textId2 == 5000)
        {
            if (config::patchCampIcons)
            {
                SetupCampsERR(rowId, row);
                modified_camp++;
            }
        }
        // Merchant markers (textId4=8800) — ERR-placed, opt-in patching
        else if (row.textId4 == 8800)
        {
            if (config::patchMerchantIcons)
            {
                SetupMerchants(rowId, row);
                modified_merchant++;
            }
        }
        // Boss markers — overworld (textId2=5100) or dungeon (textId3=5100/5300)
        else if (row.textId2 == 5100 || row.textId3 == 5100 || row.textId3 == 5300)
        {
            if (row.textId2 == 5100)
            {
                if (config::patchOverworldBossIcons)
                {
                    SetupOverworldERR(rowId, row);
                    modified_boss++;
                }
            }
            else
            {
                if (config::patchDungeonBossIcons)
                {
                    SetupDungeonERR(rowId, row);
                    modified_boss++;
                }
            }
        }
    }

    spdlog::debug("Map logic applied: {} goblin icons, {} bosses, {} camps, {} merchants",
                  modified_goblin, modified_boss, modified_camp, modified_merchant);
    spdlog::info("[BENCH] map.apply_logic.count: {} rows scanned ({} goblin, {} boss, {} camp, {} merchant)",
                 scanned_rows, modified_goblin, modified_boss, modified_camp, modified_merchant);
}
