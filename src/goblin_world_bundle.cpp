#include "goblin_world_bundle.hpp"

#include <fstream>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include "goblin_inject.hpp"      // reset_lot_reader
#include "goblin_param_edit.hpp"  // param_clone_row, param_set_field_by_name, field_is_known

namespace
{
    struct CloneOp { std::string param; uint64_t src; int32_t newId; };
    struct SetOp   { std::string param; uint64_t row; std::string field; double value; };

    std::mutex g_mtx;
    std::vector<CloneOp> g_clones;
    std::vector<SetOp> g_sets;
    std::filesystem::path g_folder;  // mod folder (for the no-arg default_path); set at boot

    std::wstring widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }  // ASCII param names
}

namespace goblin::world_bundle
{
    void record_set(const std::string &param, uint64_t row, const std::string &field, double value)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto &s : g_sets)
            if (s.param == param && s.row == row && s.field == field) { s.value = value; return; }
        g_sets.push_back({param, row, field, value});
    }

    void record_clone(const std::string &param, uint64_t src, int32_t newId)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto &c : g_clones)
            if (c.param == param && c.newId == newId) { c.src = src; return; }
        g_clones.push_back({param, src, newId});
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_clones.clear();
        g_sets.clear();
    }

    size_t op_count()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_clones.size() + g_sets.size();
    }

    std::string status_line()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return "clones=" + std::to_string(g_clones.size()) + " sets=" + std::to_string(g_sets.size());
    }

    std::filesystem::path default_path(const std::filesystem::path &mod_folder)
    {
        return mod_folder / "world_bundle.toml";
    }
    std::filesystem::path default_path()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_folder / "world_bundle.toml";
    }
    void set_folder(const std::filesystem::path &mod_folder)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_folder = mod_folder;
    }
    bool save_default() { return save(default_path()); }
    int apply_default()
    {
        std::filesystem::path path = default_path();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return 0;
        return apply(path);
    }

    bool save(const std::filesystem::path &path)
    {
        toml::array clones, sets;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (const auto &c : g_clones)
                clones.push_back(toml::table{{"param", c.param},
                                             {"src", static_cast<int64_t>(c.src)},
                                             {"new", static_cast<int64_t>(c.newId)}});
            for (const auto &s : g_sets)
                sets.push_back(toml::table{{"param", s.param},
                                           {"row", static_cast<int64_t>(s.row)},
                                           {"field", s.field},
                                           {"value", s.value}});
        }
        toml::table root;
        root.insert("clone", std::move(clones));
        root.insert("set", std::move(sets));
        try
        {
            std::ofstream os(path);
            if (!os) { spdlog::error("[WORLDBUNDLE] save: cannot open {}", path.string()); return false; }
            os << "# MapForGoblins world bundle — regulation-free World Editor edits, re-applied at boot.\n";
            os << root << "\n";
        }
        catch (const std::exception &e)
        {
            spdlog::error("[WORLDBUNDLE] save {} failed: {}", path.string(), e.what());
            return false;
        }
        spdlog::info("[WORLDBUNDLE] saved {} ({})", path.string(), status_line());
        return true;
    }

    bool load(const std::filesystem::path &path)
    {
        // Read the file ourselves (std::ifstream, the mirror of save()'s ofstream — proven to resolve
        // the path under Wine) and parse the STRING. toml::parse_file's own file open returned an empty
        // table for a valid file under Proton, so we don't use it. Exceptions ON → catch parse errors.
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            spdlog::error("[WORLDBUNDLE] load: cannot open {}", path.string());
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        toml::table root;
        try
        {
            root = toml::parse(content);
        }
        catch (const toml::parse_error &e)
        {
            spdlog::error("[WORLDBUNDLE] load {}: parse error: {}", path.string(),
                          std::string(e.description()));
            return false;
        }
        std::vector<CloneOp> clones;
        std::vector<SetOp> sets;
        if (auto *arr = root["clone"].as_array())
            for (auto &node : *arr)
                if (auto *t = node.as_table())
                {
                    CloneOp c;
                    c.param = (*t)["param"].value<std::string>().value_or("");
                    c.src = static_cast<uint64_t>((*t)["src"].value<int64_t>().value_or(0));
                    c.newId = static_cast<int32_t>((*t)["new"].value<int64_t>().value_or(0));
                    if (!c.param.empty() && c.newId) clones.push_back(std::move(c));
                }
        if (auto *arr = root["set"].as_array())
            for (auto &node : *arr)
                if (auto *t = node.as_table())
                {
                    SetOp s;
                    s.param = (*t)["param"].value<std::string>().value_or("");
                    s.row = static_cast<uint64_t>((*t)["row"].value<int64_t>().value_or(0));
                    s.field = (*t)["field"].value<std::string>().value_or("");
                    s.value = (*t)["value"].value<double>().value_or(0.0);
                    if (!s.param.empty() && !s.field.empty()) sets.push_back(std::move(s));
                }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_clones = std::move(clones);
            g_sets = std::move(sets);
        }
        spdlog::info("[WORLDBUNDLE] loaded {} ({})", path.string(), status_line());
        return true;
    }

    int apply_current()
    {
        // Snapshot under the lock, then apply outside it (param edits take their own locks).
        std::vector<CloneOp> clones;
        std::vector<SetOp> sets;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            clones = g_clones;
            sets = g_sets;
        }
        int applied = 0;
        // Clones FIRST — a set may target a freshly cloned row.
        for (const auto &c : clones)
        {
            if (goblin::paramedit::param_clone_row(widen(c.param).c_str(), c.src, c.newId))
                ++applied;
            else
                spdlog::warn("[WORLDBUNDLE] clone {}[{}]->[{}] failed (src missing / id in use?)",
                             c.param, c.src, c.newId);
        }
        for (const auto &s : sets)
        {
            if (!goblin::paramedit::field_is_known(widen(s.param).c_str(), s.field.c_str()))
            {
                spdlog::warn("[WORLDBUNDLE] set {}.{}: unknown field — skipped", s.param, s.field);
                continue;
            }
            if (goblin::paramedit::param_set_field_by_name(widen(s.param).c_str(), s.row, s.field.c_str(),
                                                           s.value))
                ++applied;
            else
                spdlog::warn("[WORLDBUNDLE] set {}[{}].{}={} failed", s.param, s.row, s.field, s.value);
        }
        // Live-applied clones added ItemLotParam rows → drop the cached LotReader so they resolve.
        if (!clones.empty())
            goblin::reset_lot_reader();
        spdlog::info("[WORLDBUNDLE] applied {} ops ({} clones, {} sets)", applied, clones.size(),
                     sets.size());
        return applied;
    }

    int apply(const std::filesystem::path &path)
    {
        if (!load(path)) return 0;
        return apply_current();
    }

    int apply_boot(const std::filesystem::path &mod_folder)
    {
        set_folder(mod_folder);
        std::filesystem::path path = default_path(mod_folder);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return 0;  // no bundle — nothing to do
        return apply(path);
    }
}
