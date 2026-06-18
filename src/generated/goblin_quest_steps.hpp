#pragma once
// Quest Browser step table. Data is HAND-AUTHORED in goblin_quest_steps.cpp
// (original descriptions; no third-party prose copied — quest facts only).
#include <cstddef>
#include <cstdint>

namespace goblin::generated
{
// One quest step. `zone` is the region name (e.g. "Altus Plateau") used later by
// Phase B to match a marker region for a "show on map" reveal; null if unknown.
struct QuestStep { const char *title; const char *desc; const char *zone; };

// One NPC questline. `related` = a short note on interconnections ("Start after
// Kenneth's quest", "Part of Ranni's questline") or null. steps==null / count==0
// means the entry is a PLACEHOLDER not yet authored.
// `dlc` flags a Shadow of the Erdtree questline (default false = base game);
// the overlay groups the browser by it. `warning` is an optional order-sensitive
// / missable note (e.g. "doing X before Y loses progress"): when set, the overlay
// tints the questline amber + a "(!)" marker and shows the note. Both are trailing
// default-init members so only the entries that need them set them in the table.
// `fail_flag` (optional): an event-flag id that, once SET, means this questline
// can no longer be completed (the NPC died early / a needed interconnection is
// gone). The overlay greys the line out when the watcher reads this flag as set.
// 0 = unknown/none. Capture a real id with the in-overlay Event-flag hook.
// `fail_conclusion` (optional): set true when `fail_flag` is the NPC's shared
// "concluded" flag — i.e. the SAME id is set on PEACEFUL completion as on the
// NPC's death (the merchant/quest NPCs whose dead flag == their `_q99` flag).
// The line still greys (no longer actionable) but the overlay labels it
// "[concluded]" with a neutral note instead of "[unfinishable] ... is dead",
// since the flag does not prove the NPC died. Default false = death-exclusive.
struct NpcQuest { const char *name; const char *quest_title; const char *related;
                  const QuestStep *steps; size_t step_count; bool dlc = false;
                  const char *warning = nullptr; uint32_t fail_flag = 0;
                  bool fail_conclusion = false; };

extern const NpcQuest QUEST_BROWSER[];
extern const size_t QUEST_BROWSER_COUNT;

} // namespace
