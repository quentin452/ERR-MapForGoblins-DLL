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

    // ── Custom items (Phase 2 strip-and-reinject) ────────────────────────────
    // A framework-granted custom item to persist across saves WITHOUT dirtying the save:
    // stripped from the live inventory right before the game serializes a save, reinjected
    // after + re-granted on world-enter. add accumulates qty (category-encoded id, e.g.
    // 0x40000000|goodsId); a total ≤0 drops the record.
    void add_custom_item(uint32_t item_id, int32_t qty);

    // Register a DECLARATIVE author-surface item (custom_items.toml), re-registered every boot.
    // Granted on world-enter + stripped pre-save exactly like add_custom_item's items, but NEVER
    // persisted to the .mfg (load/save ignore it) — the toml is its source of truth. qty<=0 removes.
    void register_author_item(uint32_t item_id, int32_t qty);
    void remove_custom_item(uint32_t item_id);
    std::vector<std::pair<uint32_t, int32_t>> custom_items();

    // ── Persistence ──────────────────────────────────────────────────────────
    // Force a load / save of the current sidecar path. save() writes atomically
    // (temp + rename). Both return false if the path is unknown or I/O failed.
    bool load();
    bool save();

    // One-line status for the RPC driver / logs: path, loaded, flag/kv counts, dirty.
    std::string status_line();

    // ── Lifecycle (slice 1b — replay on load, autosave on exit) ──────────────
    // Poll-thread tick: `world_loaded` = the caller's live "player is in the world" signal
    // (e.g. get_player_world_pos resolving). On the title→in-world EDGE it QUEUES a flag
    // replay (run on the present thread by pump_present — SetEventFlag must run there); on
    // the in-world→title EDGE it autosaves dirty state. Cheap; no-op when the feature is off.
    void tick(bool world_loaded);

    // Present-thread pump: if tick() queued a replay, re-apply the sidecar's custom event
    // flags into the live session via markers::set_event_flag (idempotent — safe to re-run).
    // Call once per frame from the present pump (next to debug_rpc::pump). No-op otherwise.
    void pump_present();

    // Install the read-only save-fn observer (Phase 2 RE — confirm the save routine before
    // wiring strip/reinject). Gated on sidecar_save; call once at init. [SAVEFN] in the log.
    void install_save_hook();

    // Clear the serialize-observer's captured caller set (RE aid — call right before triggering a
    // save so the fresh [SERFN] chains are save-specific, not boot/load noise).
    void reset_serialize_probe();
}
