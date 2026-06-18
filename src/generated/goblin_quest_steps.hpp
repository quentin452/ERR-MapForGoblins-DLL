#pragma once
// Quest Browser step table. Data is HAND-AUTHORED in goblin_quest_steps.cpp
// (original descriptions; no third-party prose copied).
#include <cstddef>

namespace goblin::generated
{
struct QuestStep { const char *title; const char *desc; };
struct NpcQuest { const char *name; const char *quest_title;
                  const QuestStep *steps; size_t step_count; };
extern const NpcQuest QUEST_BROWSER[];
extern const size_t QUEST_BROWSER_COUNT;

} // namespace
