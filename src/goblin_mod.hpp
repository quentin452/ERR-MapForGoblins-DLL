#pragma once
// goblin_mod — the MapForGoblins mod MANIFEST. One <mod_folder>/mod.toml declares the whole mod; load()
// realizes the sections it declares. See docs/plans/mod_manifest_system_plan.md.
//
// Slice 1: [mod] metadata + [style] -> postfx. No mod.toml = no-op (subsystems load via their own boot
// calls, backward-compatible). Later slices take over the worlds/bundle/items load order + [[object]].

#include <filesystem>
#include <string>

namespace goblin::mod
{
    // Parse <mod_folder>/mod.toml if present + realize its sections. Returns true iff a manifest was
    // found + parsed. Records mod_folder for a later `mod reload`.
    bool load(const std::filesystem::path &mod_folder);

    // One-line summary of the loaded manifest (name/version + what was applied) for the RPC.
    std::string status();

    // Re-parse + re-apply the recorded mod_folder's mod.toml (dev: edit + `mod reload`).
    bool reload();
}
