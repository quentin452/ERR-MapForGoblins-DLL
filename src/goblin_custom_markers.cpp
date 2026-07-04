#include "goblin_custom_markers.hpp"

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
