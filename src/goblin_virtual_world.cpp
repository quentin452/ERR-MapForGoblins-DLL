// Virtual-world registry — see goblin_virtual_world.hpp. Mutex-guarded; render reads via snapshots.

#include "goblin_virtual_world.hpp"

#include <fstream>
#include <mutex>

#include <spdlog/spdlog.h>
#define TOML_EXCEPTIONS 0  // parse errors returned as a result, not thrown — the ONLY config whose
#include <toml++/toml.hpp>  // parse works under Proton/Wine (custom_items.cpp; the exceptions-ON
                            // ifstream+parse(string) path silently returns an EMPTY table here).

namespace goblin::vworld
{
namespace
{
    std::mutex g_mtx;
    std::vector<World> g_worlds;   // custom worlds (id ≥ 1)
    int g_next_id = 1;
    int g_active = 0;              // 0 = base ER
    std::filesystem::path g_folder;

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

void set_folder(const std::filesystem::path &mod_folder)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_folder = mod_folder;
}

std::filesystem::path default_path()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_folder / "virtual_worlds.toml";
}

bool save(const std::filesystem::path &path)
{
    toml::array worlds;
    int active_snapshot = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        active_snapshot = g_active;
        for (const auto &w : g_worlds)
        {
            toml::array markers;
            for (const auto &m : w.markers)
                markers.push_back(toml::table{{"x", m.x}, {"z", m.z},
                                              {"color", static_cast<int64_t>(m.color)},
                                              {"category", static_cast<int64_t>(m.category)},
                                              {"name", m.name}});
            worlds.push_back(toml::table{{"id", static_cast<int64_t>(w.id)},
                                         {"name", w.name},
                                         {"originX", w.originX},
                                         {"originZ", w.originZ},
                                         {"scale", w.scale},
                                         {"marker", std::move(markers)}});
        }
    }
    toml::table root;
    root.insert("active", static_cast<int64_t>(active_snapshot));
    root.insert("world", std::move(worlds));
    try
    {
        std::ofstream os(path);
        if (!os) { spdlog::error("[VWORLD] save: cannot open {}", path.string()); return false; }
        os << "# MapForGoblins virtual worlds — mod-owned custom map pages (World Virtualization).\n";
        os << root << "\n";
    }
    catch (const std::exception &e)
    {
        spdlog::error("[VWORLD] save {} failed: {}", path.string(), e.what());
        return false;
    }
    spdlog::info("[VWORLD] saved {}", path.string());
    return true;
}

bool load(const std::filesystem::path &path)
{
    // TOML_EXCEPTIONS 0 + parse_file — the config custom_items.cpp proved works under Proton (the
    // exceptions-ON parse silently returns an EMPTY table on this Wine build, which is why the earlier
    // ifstream+parse(string) attempt loaded 0 worlds). See toml-parse-file-proton-bug.md.
    auto result = toml::parse_file(path.string());
    if (!result)
    {
        const auto &err = result.error();
        spdlog::error("[VWORLD] load {}: parse error: {} (line {})", path.string(),
                      std::string(err.description()), err.source().begin.line);
        return false;
    }
    const toml::table &root = result.table();
    std::vector<World> worlds;
    int max_id = 0;
    if (auto *arr = root["world"].as_array())
        for (auto &node : *arr)
            if (auto *t = node.as_table())
            {
                World w;
                w.id = static_cast<int>((*t)["id"].value<int64_t>().value_or(0));
                w.name = (*t)["name"].value<std::string>().value_or("");
                w.originX = (*t)["originX"].value<float>().value_or(0.0f);
                w.originZ = (*t)["originZ"].value<float>().value_or(0.0f);
                w.scale = (*t)["scale"].value<float>().value_or(1.0f);
                if (auto *ma = (*t)["marker"].as_array())
                    for (auto &mn : *ma)
                        if (auto *mt = mn.as_table())
                        {
                            Marker m;
                            m.x = (*mt)["x"].value<float>().value_or(0.0f);
                            m.z = (*mt)["z"].value<float>().value_or(0.0f);
                            m.color = static_cast<uint32_t>((*mt)["color"].value<int64_t>().value_or(0xFFEB82E6));
                            m.category = static_cast<int>((*mt)["category"].value<int64_t>().value_or(-1));
                            m.name = (*mt)["name"].value<std::string>().value_or("");
                            w.markers.push_back(std::move(m));
                        }
                if (w.id > 0) { if (w.id > max_id) max_id = w.id; worlds.push_back(std::move(w)); }
            }
    int active_loaded = static_cast<int>(root["active"].value<int64_t>().value_or(0));
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_worlds = std::move(worlds);
        g_next_id = max_id + 1;
        // Only honour the saved active id if that world still exists (else base ER).
        g_active = 0;
        if (active_loaded != 0)
            for (const auto &w : g_worlds)
                if (w.id == active_loaded) { g_active = active_loaded; break; }
    }
    spdlog::info("[VWORLD] loaded {} ({} worlds)", path.string(), max_id ? g_worlds.size() : 0);
    return true;
}

bool save_default() { return save(default_path()); }

int load_default()
{
    std::filesystem::path path = default_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    if (!load(path)) return 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    return static_cast<int>(g_worlds.size());
}

int load_boot(const std::filesystem::path &mod_folder)
{
    set_folder(mod_folder);
    return load_default();
}
} // namespace goblin::vworld
