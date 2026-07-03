#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Shadow / sidecar save — Phase 1 STATE STORE (docs/plans/shadow_sidecar_save_plan.md).
//
// A DLL-owned `<save>.mfg` file sitting next to the game's real save (ER0000.err on ERR /
// ER0000.sl2 on vanilla — resolved DYNAMICALLY, never hardcoded), holding framework state
// the DLL FULLY owns and the vanilla `.sl2` has no legal home for: custom event flags we
// set, per-save mod config, progress counters. Param FIELD edits do NOT belong here (they
// reload from regulation.bin each boot — the shipped param-override loader handles them).
//
// Phase 1 (this) is the file + binding + flat mINI state, with load/save driven by the
// existing CreateFileW hook (it already sees the game open the save file). It does NOT touch
// the inventory — no strip, no RemoveItem (that is Phase 2, the hard part). Opt-in via
// `[Sidecar] sidecar_save` (default OFF); every entry point is a silent no-op when off.
//
// Threading: the CreateFileW hook fires on many game threads; the RPC pump and present
// thread also touch state. All public functions take an internal mutex — callers need no
// external locking. Disk I/O happens under the lock (save events are rare).
namespace goblin::sidecar
{
    // ── Save-file detection (wired from the CreateFileW hook) ────────────────
    // The game opened a save file. `for_write` = the open requested GENERIC_WRITE
    // (a SAVE in progress) vs a read (a LOAD). First call resolves the sidecar path
    // (`<save>` with the extension swapped to `.mfg`) and LOADS an existing sidecar;
    // a write-open triggers a sidecar SAVE so the two land together. No-op when the
    // feature is off or the path doesn't look like an ER save (ER*.sl2 / ER*.err).
    void note_save_file_opened(const wchar_t *path, bool for_write);

    // Resolved sidecar file path (`<save>.mfg`), empty until a save file is seen. utf8.
    std::string sidecar_path_utf8();

    // ── State (framework-owned, persisted to the .mfg) ───────────────────────
    // A custom event flag the framework set and wants restored next load. add/remove
    // mark the store dirty (autosaved on the next game-save signal / explicit save).
    void add_custom_flag(uint32_t flag_id);
    void remove_custom_flag(uint32_t flag_id);
    bool has_custom_flag(uint32_t flag_id);
    std::vector<uint32_t> custom_flags();

    // Free-form per-save string state (progress counters, mod config). Empty get = unset.
    void set_kv(const std::string &key, const std::string &value);
    std::string get_kv(const std::string &key);

    // ── Persistence ──────────────────────────────────────────────────────────
    // Force a load / save of the current sidecar path. save() writes atomically
    // (temp + rename). Both return false if the path is unknown or I/O failed.
    bool load();
    bool save();

    // One-line status for the RPC driver / logs: path, loaded, flag/kv counts, dirty.
    std::string status_line();
}
