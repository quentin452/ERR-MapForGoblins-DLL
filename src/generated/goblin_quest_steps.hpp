#pragma once
// Quest Browser step table. Data is HAND-AUTHORED in goblin_quest_steps.cpp
// (original descriptions; no third-party prose copied — quest facts only).
#include <cstddef>

namespace goblin::generated
{
// One quest step. `zone` is the region name (e.g. "Altus Plateau") used later by
// Phase B to match a marker region for a "show on map" reveal; null if unknown.
struct QuestStep { const char *title; const char *desc; const char *zone; };

// One NPC questline. `related` = a short note on interconnections ("Start after
// Kenneth's quest", "Part of Ranni's questline") or null. steps==null / count==0
// means the entry is a PLACEHOLDER not yet authored.
struct NpcQuest { const char *name; const char *quest_title; const char *related;
                  const QuestStep *steps; size_t step_count; };

extern const NpcQuest QUEST_BROWSER[];
extern const size_t QUEST_BROWSER_COUNT;

} // namespace
