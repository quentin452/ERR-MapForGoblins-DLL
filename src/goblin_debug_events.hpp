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
    // (for flags) start the drain thread. Safe no-op if both are false or on any
    // resolve/hook/log failure.
    void initialize(const std::filesystem::path &log_path, bool hook_flags,
                    bool hook_items);
}
