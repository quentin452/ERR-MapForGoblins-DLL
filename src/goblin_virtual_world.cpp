// Virtual-world registry — see goblin_virtual_world.hpp. Mutex-guarded; render reads via snapshots.

#include "goblin_virtual_world.hpp"

#include <mutex>

namespace goblin::vworld
{
namespace
{
    std::mutex g_mtx;
    std::vector<World> g_worlds;   // custom worlds (id ≥ 1)
    int g_next_id = 1;
    int g_active = 0;              // 0 = base ER

    World *find_locked(int id)
    {
        for (auto &w : g_worlds)
            if (w.id == id) return &w;
        return nullptr;
    }
}

int create(const std::string &name)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    World w;
    w.id = g_next_id++;
    w.name = name.empty() ? ("World " + std::to_string(w.id)) : name;
    g_worlds.push_back(std::move(w));
    return g_worlds.back().id;
}

bool add_marker(int worldId, float x, float z, const std::string &name, uint32_t color, int category)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    World *w = find_locked(worldId);
    if (!w) return false;
    Marker m;
    m.x = x;
    m.z = z;
    m.color = color;
    m.category = category;
    m.name = name;
    w->markers.push_back(std::move(m));
    return true;
}

bool set_active(int id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (id != 0 && !find_locked(id)) return false;
    g_active = id;
    return true;
}

int active()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_active;
}

bool get_world(int id, World &out)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    World *w = find_locked(id);
    if (!w) return false;
    out = *w;  // snapshot copy under the lock
    return true;
}

std::vector<std::pair<int, std::string>> list()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<std::pair<int, std::string>> out;
    out.emplace_back(0, "Base ER");
    for (const auto &w : g_worlds)
        out.emplace_back(w.id, w.name);
    return out;
}

void clear()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_worlds.clear();
    g_next_id = 1;
    g_active = 0;
}
} // namespace goblin::vworld
