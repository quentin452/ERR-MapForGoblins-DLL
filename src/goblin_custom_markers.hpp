#pragma once
// Player-placed custom markers — a small shared store so BOTH the Virtual World Map (place/list/edit)
// and the minimap (draw the current world's markers) read the same data. Host-side, mutex-guarded.
// In-session for now; persistence (TOML, like virtual_worlds) is a follow-up.

#include <cstdint>
#include <string>
#include <vector>

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API — host exports these to the render DLL (hot-reload split)

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
GOBLIN_RENDER_API bool add(float wx, float wz, int group, const std::string &name, uint32_t color);
GOBLIN_RENDER_API void remove_at(size_t index);
GOBLIN_RENDER_API void clear();
GOBLIN_RENDER_API size_t count();
GOBLIN_RENDER_API size_t count_in_group(int group);
// Snapshot (copy) of all markers — callers iterate a stable copy without holding the lock.
GOBLIN_RENDER_API std::vector<Marker> snapshot();
// Mutable access for the editor (rename). Returns false if index is out of range.
GOBLIN_RENDER_API bool set_name(size_t index, const std::string &name);
} // namespace goblin::custom_markers

namespace goblin::search_marks
{
// Item-search "mark all results" highlights — a shared store so BOTH the vmap (rebuilds them on each
// search) and the minimap (draws the current-dimension hits) read the same data, like custom_markers.
// In-session, replaced per search, wiped by Clear. Distinct from the blue custom pins + the native ring.
struct Mark { float wx = 0.f, wz = 0.f; int group = 0; };
GOBLIN_RENDER_API void set(std::vector<Mark> marks);   // replace ALL (vmap writes once per search; empty clears)
GOBLIN_RENDER_API std::vector<Mark> snapshot();        // stable copy (vmap draw + minimap read)
GOBLIN_RENDER_API void clear();
GOBLIN_RENDER_API size_t count();
} // namespace goblin::search_marks

namespace goblin::death_marker
{
// The single "you died here" marker (dropped runes / bloodstain), drawn with the native MENU_MAP_DropSoul
// icon on the vmap + minimap. Set on death (get_player_map_pos), replaced by the next death, cleared on
// pickup/manual. World-frame (map-space) so no chunk->world bridge needed.
GOBLIN_RENDER_API void set(float wx, float wz, int group, int souls);
GOBLIN_RENDER_API bool get(float &wx, float &wz, int &group, int &souls);   // false = none active; souls = runes waiting
GOBLIN_RENDER_API void clear();
// Per-frame: reads player HP, and on the alive->dead edge records get_player_map_pos as the death spot.
// Call every present frame (runs during gameplay, map closed). Cheap; no-op until the HP chain resolves.
GOBLIN_RENDER_API void tick();
} // namespace goblin::death_marker
