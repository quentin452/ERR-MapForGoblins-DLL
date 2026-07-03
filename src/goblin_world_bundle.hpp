#pragma once

// World bundle (in-game World Editor slice 7 — the first brick of vision #1 "World Virtualization").
// The World Editor makes LIVE regulation-free param edits (repoint / lotItemId / clone) that vanish on
// restart. A world bundle CAPTURES those edits as a small declarative TOML file that re-applies on boot,
// so an edited world persists and can be shared. It is the same shape as custom_items.toml but for
// arbitrary param edits: an ordered set of CLONE ops (add a row by copying one) + SET ops (write a field
// by name). Applied clones-first, then sets, so a set can target a freshly cloned row. See docs/HANDOFF.md
// (World Editor / World Virtualization) and docs/runtime_modding_framework_vision.md.

#include <cstdint>
#include <filesystem>
#include <string>

namespace goblin::world_bundle
{
    // ── Record live edits into the in-memory bundle (the panel calls these after a successful edit) ──
    // Dedup: a SET keeps the last value per (param,row,field); a CLONE is unique per (param,newId).
    void record_set(const std::string &param, uint64_t row, const std::string &field, double value);
    void record_clone(const std::string &param, uint64_t src, int32_t newId);
    void clear();
    size_t op_count();                 // clones + sets currently held
    std::string status_line();         // "path=<default> clones=<n> sets=<n>"

    // ── Persist / restore ──
    bool save(const std::filesystem::path &path);   // write the in-memory bundle to a TOML file
    bool load(const std::filesystem::path &path);   // parse a TOML file INTO memory (replaces it), no apply
    int apply_current();                            // apply the in-memory bundle to live params; ops applied
    int apply(const std::filesystem::path &path);   // load(path) then apply_current() — convenience

    // Boot: apply the default world bundle in `mod_folder` if it exists (before the first marker build,
    // so the initial resolve already sees the edits — no LotReader reset needed at boot). Returns ops.
    // Also records `mod_folder` so the no-arg default_path()/save_default()/apply_default() work later.
    int apply_boot(const std::filesystem::path &mod_folder);
    void set_folder(const std::filesystem::path &mod_folder);
    std::filesystem::path default_path();                              // <folder>/world_bundle.toml
    std::filesystem::path default_path(const std::filesystem::path &mod_folder);
    bool save_default();    // save() to default_path()
    int apply_default();    // apply() from default_path() (no-op / 0 if absent)
}
