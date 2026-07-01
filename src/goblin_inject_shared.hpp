#pragma once
//
// Tiny cross-TU accessor surface for state that stays owned in one split file
// but is also needed by another. Mirrors the src/input/input_shared.hpp
// pattern: the owning globals are NOT moved, just exposed via a thin getter
// so the other side doesn't need its own copy.
//
#include <cstdint>

// Owned by goblin_inject.cpp (the "either-flag kill indicators" event-flag
// query); needed by goblin_loot_resolve.cpp's live-persistence classifier.
bool orp_flag_set(uint32_t flag_id);
bool orp_manager_resolve_tried();
void **orp_event_man_slot_ptr();

// Owned by goblin_section_visibility.cpp (PR 3); needed by goblin_inject.cpp's
// menu_auto_toggle_loop (a generic watcher thread that stayed behind).
bool icons_user_disabled();
int take_section_apply_req();  // exchanges the pending section-apply request for -1
void persist_settings();       // syncs live visibility/clustering state into config + saves ini
