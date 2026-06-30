#pragma once
// QuestNpcLayer — pins the NPC/asset of each questline's active (first not-done) step
// on the world map. Sole producer of Category::WorldQuestNPC markers (the old
// map_entry_layer.cpp bake-loop emission is dead, see generated_data_removal_plan.md).

#include "marker_layer.hpp"

namespace goblin::worldmap
{
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
