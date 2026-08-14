#include "goblin_logic.hpp"
#include "goblin/goblin_structs.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "goblin/goblin_map_tiles.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

using namespace goblin;
using namespace goblin::mapPoint;

// ── Active event-flag set (2026-08-14) ────────────────────────────────────────
// Every flag the ACTIVE install's EMEVDs SetEventFlag(., state=1) — fed by
// load_emevd_awards' setter scan (loot_disk), published once after the first pass.
// The gate tables (fragment flags, story flags) are vanilla-derived; honouring a
// table flag only when the active mod actually sets it turns a renumbering mod's
// silent "everything hidden forever" into an honest "ungated" (or keeps the gate
// when the mod genuinely implements it — GA sets the same 62010-62084 ids).
namespace
{
std::mutex                       g_active_mx;
std::unordered_set<uint32_t>     g_active_set;
std::atomic<bool>                g_active_ready{false};
} // namespace

void goblin::note_active_event_flags(const std::vector<uint32_t> &flags)
{
    std::lock_guard<std::mutex> lk(g_active_mx);
    for (uint32_t f : flags) g_active_set.insert(f);
}

void goblin::publish_active_event_flags()
{
    std::lock_guard<std::mutex> lk(g_active_mx);
    g_active_ready.store(true, std::memory_order_release);
}

bool goblin::active_event_flag(uint32_t flag)
{
    if (!g_active_ready.load(std::memory_order_acquire))
        return true;  // EMEVD scan not run yet (pass off / build pending) — assume the tables are right
    std::lock_guard<std::mutex> lk(g_active_mx);
    return g_active_set.count(flag) != 0;
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

    // Hand-authored-MapList gap fill (overworld only). The table was built for the static
    // bake's marker set; the no-bake disk pass now places markers on a few INTERIOR overworld
    // tiles the table omits. Those return 0 here ("no fragment → always shown"), so they LEAK
    // past require_map_fragments (visible even without the region's map). For an overworld tile
    // with no exact match, inherit the MAJORITY map-fragment of the nearest covered ring:
    // ±1 neighbours first, then ±2 (the ±1-only fill missed off-shore islet tiles — Divine
    // Towers etc. — which leaked, user-reported 2026-07-02). A tile with no covered neighbour
    // within 2 (deep ocean / an area the table doesn't cover) stays 0 and keeps the
    // always-shown default. Build-time only (push_marker / cluster labels), never per-frame.
    if (location.X == 60 || location.X == 61)
    {
        for (int radius = 1; radius <= 2; ++radius)
        {
            int ids[24], cnt = 0;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dz = -radius; dz <= radius; ++dz)
                {
                    if (std::max(std::abs(dy), std::abs(dz)) != radius) continue; // ring only
                    MapTile nb(location.X, location.Y + dy, location.Z + dz);
                    bool found = false;
                    for (const auto &fragment : MapList)
                    {
                        for (const auto &chunk : fragment.mapFragmentTile)
                            if (chunk == nb) { ids[cnt++] = fragment.mapFragmentId; found = true; break; }
                        if (found) break;
                    }
                }
            int best_id = 0, best_n = 0;
            for (int i = 0; i < cnt; ++i)
            {
                int n = 0;
                for (int j = 0; j < cnt; ++j)
                    if (ids[j] == ids[i]) ++n;
                if (n > best_n) { best_n = n; best_id = ids[i]; }
            }
            if (best_id)
                return best_id;
        }
        return 0;
    }
    return 0;
}

// Map-fragment discovery flag for a tile — the overlay gates markers behind
// require_map_fragments on this (0 = tile needs no fragment). The per-paramId
// ExceptionList overrides aren't applied here (no rowId at the marker layer); the tile
// table covers the overwhelming majority. HARDENED 2026-08-14: a tile's table flag is
// honoured only when the ACTIVE install's EMEVDs set it (active_event_flag) — a mod
// that renumbers its fragment flags degrades to "ungated" instead of hiding every
// region forever (and a table flag the mod never sets, e.g. ERR's 62008 FarumAzula,
// stops hiding that region permanently).
int goblin::map_fragment_flag(int area, int gx, int gz)
{
    const int flag = GetMapFlagFromTile(MapTile(area, gx, gz));
    return (flag != 0 && goblin::active_event_flag(static_cast<uint32_t>(flag))) ? flag : 0;
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
