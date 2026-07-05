#define TOML_EXCEPTIONS 0  // parse errors as a result, not thrown — the ONLY toml++ path that works under
#include <toml++/toml.hpp>  // Proton/Wine (see toml-parse-file-proton-bug.md; matches goblin_virtual_world).

#include "goblin_mod.hpp"
#include "goblin_postfx.hpp"
#include "goblin_custom_items.hpp"
#include "goblin_world_bundle.hpp"
#include "goblin_virtual_world.hpp"

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

    // A section is ON unless the manifest declares it with enabled=false. Absent section (or no manifest)
    // => default ON, so behaviour matches the pre-manifest boot exactly (backward-compatible).
    bool section_on(const toml::table *root, const char *key)
    {
        if (!root) return true;
        if (auto *s = (*root)[key].as_table()) return (*s)["enabled"].value<bool>().value_or(true);
        return true;
    }

    // Slice 2: the manifest OWNS the load order. Realize the sub-systems in the required order
    // (items -> bundle -> worlds; the ordering/timing constraints the old dllmain init_* calls had), then
    // [style]. `root` = the parsed mod.toml table, or null if there is no manifest (=> load everything).
    void orchestrate(const toml::table *root, const std::filesystem::path &folder, bool boot)
    {
        std::string on;
        // The sub-loaders run ONLY at boot (they grant items / apply param edits / load the registry — not
        // safe to re-run on a live `mod reload`, which re-applies [style] only).
        if (boot)
        {
            // items — custom_items.toml (after setup_messages/params ready, which is where init_mod now runs).
            if (section_on(root, "items"))  { goblin::custom_items::apply(folder);   on += " items"; }
            // bundle — world_bundle.toml (AEG edits); after items (params ready), before the first marker build.
            if (section_on(root, "bundle")) { goblin::world_bundle::apply_boot(folder); on += " bundle"; }
            // worlds — virtual_worlds.toml (mod-owned map pages); data-only, after bundle.
            if (section_on(root, "worlds")) { goblin::vworld::load_boot(folder);     on += " worlds"; }
        }
        else
            on = " (reload: style only)";

        // [style] -> postfx (greybox #2b), applied at boot.
        std::string styled;
        if (root)
            if (auto *st = (*root)["style"].as_table())
            {
                bool en = (*st)["enabled"].value<bool>().value_or(false);
                std::string mode = (*st)["mode"].value<std::string>().value_or("grayscale");
                float strength = (*st)["strength"].value<float>().value_or(1.0f);
                int mi = style_mode(mode);
                if (mi > 0) { goblin::postfx::set_mode(mi); goblin::postfx::set_strength(strength); }
                else spdlog::warn("[MOD] [style] unknown mode '{}'", mode);
                goblin::postfx::set_enabled(en && mi > 0);
                if (en && mi > 0) styled = " style=" + mode;
            }

        std::string name, version;
        if (root)
            if (auto *m = (*root)["mod"].as_table())
            {
                name = (*m)["name"].value<std::string>().value_or("");
                version = (*m)["version"].value<std::string>().value_or("");
            }
        g_summary = (root ? ("name='" + name + "' v" + version) : "(no mod.toml)") +
                    " loaded:" + (on.empty() ? " none" : on) + styled;
        spdlog::info("[MOD] {}", g_summary);
    }

    bool run(const std::filesystem::path &folder, bool boot)
    {
        std::filesystem::path path = folder / "mod.toml";
        std::error_code ec;
        bool has = std::filesystem::exists(path, ec);
        auto res = has ? toml::parse_file(path.string()) : toml::parse_result{};
        const toml::table *root = nullptr;
        if (has && !res)
            spdlog::error("[MOD] parse {}: {} (line {})", path.string(),
                          std::string(res.error().description()), res.error().source().begin.line);
        else if (has)
            root = &res.table();
        orchestrate(root, folder, boot);   // no manifest => load everything (backward-compatible)
        return has && root != nullptr;
    }
}

namespace goblin::mod
{
    bool load(const std::filesystem::path &mod_folder)
    {
        g_folder = mod_folder;
        return run(mod_folder, /*boot=*/true);
    }
    std::string status() { return "ok mod " + g_summary; }
    bool reload()
    {
        if (g_folder.empty()) { g_summary = "(no folder set)"; return false; }
        return run(g_folder, /*boot=*/false);   // dev: re-apply [style] only (not the boot-only sub-loaders)
    }
}
