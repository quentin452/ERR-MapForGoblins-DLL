#include "goblin_custom_markers.hpp"
#include "goblin_inventory.hpp"   // read_bloodstain — the game's OWN persistent death marker

#include <mutex>

namespace goblin::custom_markers
{
namespace
{
std::mutex g_mtx;
std::vector<Marker> g_markers;
}

bool add(float wx, float wz, int group, const std::string &name, uint32_t color)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = 0;
    for (const Marker &m : g_markers)
        if (m.group == group) ++n;
    if (n >= kMaxPerGroup) return false;   // per-world cap reached
    g_markers.push_back(Marker{wx, wz, group, name, color});
    return true;
}

void remove_at(size_t index)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (index < g_markers.size()) g_markers.erase(g_markers.begin() + index);
}

void clear()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_markers.clear();
}

size_t count()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_markers.size();
}

size_t count_in_group(int group)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    size_t n = 0;
    for (const Marker &m : g_markers)
        if (m.group == group) ++n;
    return n;
}

std::vector<Marker> snapshot()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_markers;
}

bool set_name(size_t index, const std::string &name)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (index >= g_markers.size()) return false;
    g_markers[index].name = name;
    return true;
}
} // namespace goblin::custom_markers

namespace goblin::death_marker
{
namespace
{
std::mutex g_dm_mtx;
bool g_active = false;
float g_wx = 0.f, g_wz = 0.f;
int g_group = 0;
}
void set(float wx, float wz, int group)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    g_active = true; g_wx = wx; g_wz = wz; g_group = group;
}
bool get(float &wx, float &wz, int &group)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    if (!g_active) return false;
    wx = g_wx; wz = g_wz; group = g_group;
    return true;
}
void clear()
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    g_active = false;
}
void tick()
{
    // Mirror the game's OWN persistent bloodstain (GameDataMan+0x48) — save-backed, so it survives
    // restart + auto-clears when the runes are collected, EXACTLY like ER (no HP-edge guessing, no hook).
    float x = 0.f, y = 0.f, z = 0.f; uint32_t mapid = 0; int32_t souls = 0;
    if (!goblin::inventory::read_bloodstain(x, y, z, mapid, souls)) return; // not resolvable (load/menu)
    if (souls <= 0) { clear(); return; }                                    // none / collected
    const int area = (mapid >> 24) & 0xFF, gx = (mapid >> 16) & 0xFF, gz = (mapid >> 8) & 0xFF;
    int group;
    if (area == 60) group = 0;        // overworld
    else if (area == 61) group = 2;   // DLC overworld
    else return;                      // underground / legacy dungeon → needs WorldMapLegacyConvParam fold (TODO)
    // Marker frame = gridX*256 + local (same as graces/markers: marker_cluster_key).
    set(gx * 256.0f + x, gz * 256.0f + z, group);
}
} // namespace goblin::death_marker
