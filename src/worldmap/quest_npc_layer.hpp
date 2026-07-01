#pragma once
// QuestNpcLayer — pins the NPC/asset of each questline's active (first not-done) step
// on the world map. Sole producer of Category::WorldQuestNPC markers (the old
// map_entry_layer.cpp bake-loop emission is dead, see generated_data_removal_plan.md).

#include "marker_layer.hpp"
#include <string>
#include <vector>

namespace goblin::worldmap
{
// A quest NPC found at RUNTIME in the active mod's EMEVD (load_quest_npcs) that is NOT
// covered by a hand-authored QUEST_BROWSER entry's fail_flag — a fallback CANDIDATE. The
// disk worker fills the disk-side fields ONLY (`npcParamId` from the MSB); the NAME is
// resolved LAZILY on the render thread (npc_team_and_name reads LIVE NpcParam, which is
// not ready on the early disk worker — that's why it must be deferred). The Quest Browser
// resolves the name + the secondary name-coverage there, and shows the survivors as a
// minimal fallback (name + live [concluded] state, no step prose) — the mod-agnostic quest
// analogue of the circle fallback. `concluded` = _q99 flag; [regLo,regHi] = state register;
// `pinEntity` = a placement to pin. Written on the disk worker, read on render (mutex swap).
struct QuestFallbackNpc
{
    uint32_t concluded = 0, regLo = 0, regHi = 0, pinEntity = 0;
    // NpcParam ids of ALL the candidate's placements (from the MSB). The render tries each
    // until one has a real NpcName (nameId > 0) — a quest NPC's first placement is often a
    // logic param with nameId 0; the named row is a different phase's placement.
    std::vector<uint32_t> npcParamIds;
    std::string name;  // empty from the disk worker; filled at render
};
std::vector<QuestFallbackNpc> quest_fallback_npcs();          // render-thread: returns a copy
void set_quest_fallback_npcs(std::vector<QuestFallbackNpc> v); // disk-worker: replaces the set


class QuestNpcLayer : public MarkerLayer
{
public:
    const char *category() const override { return "World - Quest NPC"; }
    bool visible() const override;                       // master show_quest_npc + questNpcQuestAware
    const std::vector<Marker> &markers() const override; // returns the cached vector, rebuilt on demand

private:
    mutable std::vector<Marker> cache_;
    // Cheap signature of "what could change the active step": questProgress content,
    // questNpcQuestAware, and the live value of every QuestStep::progress_flag in the
    // roster (reading a few dozen event flags once per markers() call is NOT the same
    // cost class as the per-marker render hot loop -- this only runs when the layer is
    // actually queried, i.e. roughly once per frame while the map is open, not once per
    // marker). Rebuild only when the signature changes from the last build.
    mutable size_t built_sig_ = ~size_t{0};
};
} // namespace goblin::worldmap
