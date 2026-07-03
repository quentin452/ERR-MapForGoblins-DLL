#pragma once
// Virtual-world registry — the framework's own custom "dev worlds" (World Virtualization vision #1).
//
// A virtual world is a MOD-OWNED map page (docs/plans/virtual_world_multi_world_design.md): its own
// marker set in its OWN coordinate namespace, shown on the virtual map (panel_virtual_map) when it is the
// ACTIVE world. Because each world stores markers in its own space (authors place RELATIVE to the world's
// origin, the FRAMEWORK owns absolute placement), two worlds can never collide — no engine coordinates are
// involved for a marker-only world. active() == 0 means "base ER" (the virtual map falls back to the live
// ER markers by group); ids ≥ 1 are custom worlds.
//
// Thread-safe: the RPC/host mutates, the render (present) thread reads via get_world() snapshots.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace goblin::vworld
{
struct Marker
{
    float x = 0.0f, z = 0.0f;         // world-relative position (this world's own namespace)
    uint32_t color = 0xFFEB82E6u;     // packed ImU32 (ABGR) dot colour
    int category = -1;                // optional Category (for a future icon draw); -1 = none
    std::string name;                 // optional label
};

struct World
{
    int id = 0;
    std::string name;
    float originX = 0.0f, originZ = 0.0f, scale = 1.0f;  // reserved per-world projection (C1: identity)
    std::vector<Marker> markers;
};

// Create a new custom world; returns its id (≥1). The framework owns the id/namespace, not the caller.
int create(const std::string &name);
// Append a marker to world `worldId` (world-relative coords). false if the id is unknown.
bool add_marker(int worldId, float x, float z, const std::string &name, uint32_t color, int category = -1);
// Set the ACTIVE world (0 = base ER). false if id unknown (and ≠ 0).
bool set_active(int id);
int active();
// Snapshot-copy world `id` (thread-safe read for the render thread). false if unknown.
bool get_world(int id, World &out);
// (id, name) pairs for the selector — index 0 is always the synthetic {0,"Base ER"}.
std::vector<std::pair<int, std::string>> list();
// Remove all custom worlds + reset active to 0 (base ER).
void clear();

// ── Persistence (bundle-backed, TOML — slice C3) ──────────────────────────────────────────────────
// Records `mod_folder` so the no-arg default_path()/save_default()/load_boot() work later.
void set_folder(const std::filesystem::path &mod_folder);
std::filesystem::path default_path();                 // <mod_folder>/virtual_worlds.toml
bool save(const std::filesystem::path &path);         // write the registry (+ active id) to a TOML file
bool load(const std::filesystem::path &path);         // parse a TOML file INTO the registry (REPLACES it)
bool save_default();                                  // save() to default_path()
int load_default();                                   // load default_path() if present (folder already set)
int load_boot(const std::filesystem::path &mod_folder);  // set_folder + load default if present; #worlds
} // namespace goblin::vworld
