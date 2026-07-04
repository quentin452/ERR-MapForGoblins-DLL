#pragma once
// Player-placed custom markers — a small shared store so BOTH the Virtual World Map (place/list/edit)
// and the minimap (draw the current world's markers) read the same data. Host-side, mutex-guarded.
// In-session for now; persistence (TOML, like virtual_worlds) is a follow-up.

#include <cstdint>
#include <string>
#include <vector>

namespace goblin::custom_markers
{
struct Marker
{
    float wx = 0.f, wz = 0.f;   // world XZ (unified marker frame)
    int group = 0;              // which map: 0 base-overworld, 1 base-ug, 2 DLC-ow, 3 DLC-ug
    std::string name;
    uint32_t color = 0;         // IM_COL32 packed
};

constexpr int kMaxPerGroup = 24;   // cap per world (like ER's placed-marker limit)

// Add a marker (enforces the per-group cap: drops the request + returns false when full).
bool add(float wx, float wz, int group, const std::string &name, uint32_t color);
void remove_at(size_t index);
void clear();
size_t count();
size_t count_in_group(int group);
// Snapshot (copy) of all markers — callers iterate a stable copy without holding the lock.
std::vector<Marker> snapshot();
// Mutable access for the editor (rename). Returns false if index is out of range.
bool set_name(size_t index, const std::string &name);
} // namespace goblin::custom_markers

namespace goblin::death_marker
{
// The single "you died here" marker (dropped runes / bloodstain), drawn with the native MENU_MAP_DropSoul
// icon on the vmap + minimap. Set on death (get_player_map_pos), replaced by the next death, cleared on
// pickup/manual. World-frame (map-space) so no chunk->world bridge needed.
void set(float wx, float wz, int group, int souls);
bool get(float &wx, float &wz, int &group, int &souls);   // false = none active; souls = runes waiting
void clear();
// Per-frame: reads player HP, and on the alive->dead edge records get_player_map_pos as the death spot.
// Call every present frame (runs during gameplay, map closed). Cheap; no-op until the HP chain resolves.
void tick();
} // namespace goblin::death_marker
