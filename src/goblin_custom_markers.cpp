#include "goblin_custom_markers.hpp"
#include "goblin_inventory.hpp"   // read_bloodstain — the game's OWN persistent death marker
#include "goblin_inject.hpp"      // marker_world_pos / marker_group_from — same projection as real markers

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

namespace goblin::search_marks
{
namespace
{
std::mutex g_mtx;
std::vector<Mark> g_marks;
}

void set(std::vector<Mark> marks)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_marks = std::move(marks);
}
std::vector<Mark> snapshot()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_marks;
}
void clear()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_marks.clear();
}
size_t count()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_marks.size();
}
} // namespace goblin::search_marks

namespace goblin::death_marker
{
namespace
{
std::mutex g_dm_mtx;
bool g_active = false;
float g_wx = 0.f, g_wz = 0.f;
int g_group = 0, g_souls = 0;
int g_area = -1, g_gx = 0, g_gz = 0;   // raw (pre-fold) for the native-map converter; area<0 = unavailable
float g_px = 0.f, g_pz = 0.f;
bool g_manual = false;                  // set by the death_mark RPC → tick() won't auto-clear/overwrite it
}
void set(float wx, float wz, int group, int souls, int rawArea, int rawGx, int rawGz, float rawPx, float rawPz, bool manual)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    g_active = true; g_wx = wx; g_wz = wz; g_group = group; g_souls = souls;
    g_area = rawArea; g_gx = rawGx; g_gz = rawGz; g_px = rawPx; g_pz = rawPz;
    g_manual = manual;
}
bool get(float &wx, float &wz, int &group, int &souls)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    if (!g_active) return false;
    wx = g_wx; wz = g_wz; group = g_group; souls = g_souls;
    return true;
}
bool get_raw(int &area, int &gx, int &gz, float &px, float &pz)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    if (!g_active || g_area < 0) return false;
    area = g_area; gx = g_gx; gz = g_gz; px = g_px; pz = g_pz;
    return true;
}
bool state(bool &manual, float &wx, float &wz, int &group, int &souls,
           int &rawArea, int &gx, int &gz, float &px, float &pz)
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    manual = g_manual; wx = g_wx; wz = g_wz; group = g_group; souls = g_souls;
    rawArea = g_area; gx = g_gx; gz = g_gz; px = g_px; pz = g_pz;
    return g_active;
}
void clear()
{
    std::lock_guard<std::mutex> lk(g_dm_mtx);
    g_active = false; g_manual = false;
}
void tick()
{
    // A MANUAL marker (death_mark RPC) is sticky — don't let the auto-mirror below clear/overwrite it.
    // Without this, death_mark is useless when there is no real bloodstain: read_bloodstain reports
    // exists=false → the clear() branch wipes the manual marker every frame before any surface draws it.
    { std::lock_guard<std::mutex> lk(g_dm_mtx); if (g_manual) return; }
    // Mirror the game's OWN persistent bloodstain (GameDataMan+0x48) — save-backed, so it survives
    // restart + auto-clears when the runes are collected, EXACTLY like ER (no HP-edge guessing, no hook).
    // Existence = the ENGINE's flag (GameDataMan+0x40), NOT souls>0: a 0-rune death leaves a real
    // stain (souls=0) that ER's native map draws — gating on souls>0 hid it (2026-07-08 bug).
    bool exists = false;
    float x = 0.f, y = 0.f, z = 0.f; uint32_t mapid = 0; int32_t souls = 0;
    if (!goblin::inventory::read_bloodstain(exists, x, y, z, mapid, souls)) return; // not resolvable (load/menu)
    if (!exists) { clear(); return; }                                               // none / collected
    // mapId = m{AA}_{BB}_{CC}_{DD} → areaNo/gridX/gridZ. Project through the SAME pipeline as real markers
    // (marker_world_pos applies WorldMapLegacyConvParam) so underground + legacy dungeon deaths land right,
    // not just the overworld. group = which map page (marker_group_from).
    const uint8_t areaNo = (mapid >> 24) & 0xFF, gx = (mapid >> 16) & 0xFF, gz = (mapid >> 8) & 0xFF;
    int out_area = 0; float wx = 0.f, wz = 0.f;
    if (!goblin::marker_world_pos(areaNo, gx, gz, x, z, out_area, wx, wz, /*conv_underground=*/true)) return;
    // Store the folded WORLD frame (vmap/minimap) AND the RAW pre-fold (area,grid,local) so the native map
    // can run the engine converter itself — group-correct on underground/DLC pages, not just the overworld.
    set(wx, wz, goblin::marker_group_from(areaNo, out_area), souls, areaNo, gx, gz, x, z);
}
} // namespace goblin::death_marker
