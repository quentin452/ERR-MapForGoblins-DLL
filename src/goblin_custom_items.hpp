#pragma once

#include <filesystem>

// Custom-item author surface (Gap C step 4). Reads a declarative `custom_items.toml` from the mod
// folder and applies each item end-to-end using the shipped primitives — no code, no RPC:
//   param_clone_row(template → id) + param_set_field_by_name(fields) + inject_fmg_entries(name) +
//   sidecar::add_custom_item(encoded id) so the item is granted on world-enter and stripped from the
//   vanilla save (Variant A clean-save).
//
// TOML (marzer/tomlplusplus), array-of-tables per category:
//   [[goods]]
//   id    = 8000000            # base row id, MUST be <= 0x7FFFFE to be grantable (goods)
//   clone = 100                # template row to clone from (the active install's own row)
//   name  = "Goblin Test Item"
//   qty   = 1                  # optional (default 1)
//   fields = { sortGroupId = 101, weight = 0.5 }   # optional, applied by name
// Categories: goods / weapon / protector / accessory (each maps to its EquipParam*, name FMG base
// slot, and item-id encoding). DEFINE (clone+fields+name) is re-applied every boot (params/FMG reload
// from regulation each launch); the sidecar re-grants each load. Mod-agnostic: clones the ACTIVE
// install's template row.
namespace goblin::custom_items
{
    // Parse <mod_folder>/custom_items.toml and apply every declared item. Boot-time — call after
    // params AND messages are ready. No-op (returns 0) if the file is absent. Never throws; a bad
    // file or item is logged ([CUSTOMITEM]) and skipped. Returns the number of items applied.
    int apply(const std::filesystem::path &mod_folder);
}
