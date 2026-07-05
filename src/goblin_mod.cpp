#define TOML_EXCEPTIONS 0  // parse errors as a result, not thrown — the ONLY toml++ path that works under
#include <toml++/toml.hpp>  // Proton/Wine (see toml-parse-file-proton-bug.md; matches goblin_virtual_world).

#include "goblin_mod.hpp"
#include "goblin_postfx.hpp"

#include <spdlog/spdlog.h>

namespace
{
    std::filesystem::path g_folder;
    std::string g_summary = "(not loaded)";

    int style_mode(const std::string &s)
    {
        if (s == "grayscale" || s == "gray") return 1;
        if (s == "posterize") return 2;
        if (s == "edge") return 3;
        if (s == "edge_desat" || s == "edge+desat") return 4;
        return 0;
    }

    bool parse_and_apply(const std::filesystem::path &folder)
    {
        std::filesystem::path path = folder / "mod.toml";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            g_summary = "(no mod.toml)";
            return false;
        }
        auto res = toml::parse_file(path.string());
        if (!res)
        {
            spdlog::error("[MOD] parse {}: {} (line {})", path.string(),
                          std::string(res.error().description()), res.error().source().begin.line);
            g_summary = "(parse error)";
            return false;
        }
        const toml::table &root = res.table();

        std::string name, version;
        if (auto *m = root["mod"].as_table())
        {
            name = (*m)["name"].value<std::string>().value_or("");
            version = (*m)["version"].value<std::string>().value_or("");
        }

        std::string applied;
        if (auto *st = root["style"].as_table())
        {
            bool en = (*st)["enabled"].value<bool>().value_or(false);
            std::string mode = (*st)["mode"].value<std::string>().value_or("grayscale");
            float strength = (*st)["strength"].value<float>().value_or(1.0f);
            int mi = style_mode(mode);
            if (mi > 0)
            {
                goblin::postfx::set_mode(mi);
                goblin::postfx::set_strength(strength);
            }
            else
                spdlog::warn("[MOD] [style] unknown mode '{}' (grayscale|posterize|edge|edge_desat)", mode);
            goblin::postfx::set_enabled(en && mi > 0);
            if (en && mi > 0) applied += " style=" + mode;
        }

        g_summary = "name='" + name + "' v" + version + (applied.empty() ? " (no active sections)" : applied);
        spdlog::info("[MOD] loaded {}", g_summary);
        return true;
    }
}

namespace goblin::mod
{
    bool load(const std::filesystem::path &mod_folder)
    {
        g_folder = mod_folder;
        return parse_and_apply(mod_folder);
    }

    std::string status() { return "ok mod " + g_summary; }

    bool reload()
    {
        if (g_folder.empty()) { g_summary = "(no folder set)"; return false; }
        return parse_and_apply(g_folder);
    }
}
