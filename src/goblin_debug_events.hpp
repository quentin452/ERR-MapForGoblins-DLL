#pragma once
#include <filesystem>

// Thread 7 — runtime event-flag observer (debug / coverage-gap discovery).
//
// Detours the game's SetEventFlag (the "EventFlag_C1" routine, AOB lifted from
// the Hexinton all-in-one CT v6.0) so we see EVERY event flag the game sets at
// runtime: item-acquisition flags, boss/enemy-defeat flags, quest-step flags.
// The first slice just LOGS each newly-seen flag id+value to a dedicated file so
// we can observe which flags fire on pickup / kill / quest — the raw data needed
// to later classify "this item/entity/quest is unknown to the mod" and toast it.
//
// Strictly opt-in (config debug_event_flags, default false). Self-disables on any
// resolve/hook failure — the mod continues unaffected.
namespace goblin::debug_events
{
    // Resolve SetEventFlag by AOB, install the observer detour, open the log at
    // `log_path`, and start the background drain thread. Safe no-op on failure.
    void initialize(const std::filesystem::path &log_path);
}
