#pragma once
#include <filesystem>

// Thread 7 — runtime observers for coverage-gap discovery (debug).
//
// Two opt-in detours (AOBs lifted from the Hexinton all-in-one CT v6.0), each
// logging to a shared file so we can observe what the player does vs what the
// map shows:
//   * SetEventFlag ("EventFlag_C1") — every event flag the game sets (item /
//     boss-defeat / quest flags). High volume → deduped first-set-only via a
//     background drain thread.
//   * AddItemFunc — every inventory grant (pickup / shop / reward). Low volume →
//     logged raw so a controlled pickup can pin the item-id offset.
//
// Strictly opt-in (config debug_event_flags / debug_item_grants, both default
// false). Self-disables on any resolve/hook failure — the mod continues.
namespace goblin::debug_events
{
    // Open the log at `log_path`, install whichever observers are requested, and
    // (for coverage flags) start the drain thread. `hook_capture` installs the
    // SetEventFlag detour in a LIGHT mode (no drain) for the flag-capture tool.
    // Safe no-op if all are false or on any resolve/hook/log failure.
    void initialize(const std::filesystem::path &log_path, bool hook_flags,
                    bool hook_items, bool hook_capture = false);

    // ── Flag-capture (Part 2 death-flag discovery) ──────────────────────────
    // Workflow: arm (naming the NPC) → kill it in-game → wait a few seconds for
    // transient flags to clear → finalize. finalize re-checks every captured flag
    // via `reader` (the live event-flag reader) and appends the STILL-set ones to
    // logs/MapForGoblins_flagcapture.txt, tagging quest-namespace fail_flag
    // candidates. Requires the capture hook installed (config debug_flag_capture).
    void arm_capture(const char *npc_name);
    bool capture_armed();
    size_t capture_count();
    int finalize_capture(bool (*reader)(uint32_t));
}
