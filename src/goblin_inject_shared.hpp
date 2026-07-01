#pragma once
//
// Tiny cross-TU accessor surface for state that stays owned by goblin_inject.cpp
// (the "either-flag kill indicators" event-flag query) but is also needed by
// goblin_loot_resolve.cpp's live-persistence classifier. Mirrors the
// src/input/input_shared.hpp pattern: the owning globals are NOT moved, just
// exposed read-only so the split file doesn't need its own copy of the
// EventFlagMan AOB resolve.
//
#include <cstdint>

bool orp_flag_set(uint32_t flag_id);
bool orp_manager_resolve_tried();
void **orp_event_man_slot_ptr();
