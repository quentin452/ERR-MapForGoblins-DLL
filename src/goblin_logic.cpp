#include "goblin_logic.hpp"
#include "goblin/goblin_structs.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "goblin/goblin_map_tiles.hpp"

using namespace goblin;
using namespace goblin::mapPoint;

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

// Map-fragment discovery flag for a tile — the overlay gates markers behind
// require_map_fragments on this (0 = tile needs no fragment). The per-paramId
// ExceptionList overrides aren't applied here (no rowId at the marker layer); the tile
// table covers the overwhelming majority.
int goblin::map_fragment_flag(int area, int gx, int gz)
{
    return GetMapFlagFromTile(MapTile(area, gx, gz));
}

// Region name for a cluster tile, via the map-fragment grouping (the same tile→
// fragment map used for fragment-eviction). Coarse (~26 regions). "" if the tile maps
// nowhere.
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
