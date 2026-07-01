#include "quest_npc_layer.hpp"

#include "category_meta.hpp"      // category_color
#include "map_entry_layer.hpp"    // entity_world_pos
#include "goblin_map_data.hpp"    // goblin::generated::Category
#include "goblin_inject.hpp"      // goblin::ui::category_visible/section_visible/category_section
#include "goblin_quest_steps.hpp" // goblin::generated::QUEST_BROWSER, goblin::quest_step_done
#include "goblin_config.hpp"      // config::questProgress / questNpcQuestAware
#include "goblin_bench.hpp"       // GOBLIN_BENCH scoped timers

#include <functional>
#include <mutex>

namespace goblin::worldmap
{
// Runtime fallback quest NPCs (see quest_npc_layer.hpp). Written on the disk worker,
// read on the render thread; the getter returns a copy so the render side never holds
// a reference across a swap.
static std::mutex g_qfb_mtx;
static std::vector<QuestFallbackNpc> g_qfb;
std::vector<QuestFallbackNpc> quest_fallback_npcs()
{
    std::lock_guard<std::mutex> lk(g_qfb_mtx);
    return g_qfb;
}
void set_quest_fallback_npcs(std::vector<QuestFallbackNpc> v)
{
    std::lock_guard<std::mutex> lk(g_qfb_mtx);
    g_qfb = std::move(v);
}

bool QuestNpcLayer::visible() const
{
    const int g = static_cast<int>(goblin::generated::Category::WorldQuestNPC);
    return goblin::ui::category_visible(g) &&
           goblin::ui::section_visible(goblin::ui::category_section(g));
}

const std::vector<Marker> &QuestNpcLayer::markers() const
{
    using namespace goblin::generated;

    // Cheap invalidation signature -- see the .hpp comment on built_sig_. Reading a
    // few dozen event flags once per markers() call (not per marker) is fine; this is
    // NOT the per-frame render hot loop the plan warns against re-reading flags in.
    size_t sig = std::hash<std::string>{}(goblin::config::questProgress);
    sig = sig * 1000003u + (goblin::config::questNpcQuestAware ? 1 : 0);
    for (size_t qi = 0; qi < QUEST_BROWSER_COUNT; qi++)
    {
        const NpcQuest &q = QUEST_BROWSER[qi];
        for (size_t s = 0; s < q.step_count; s++)
            if (q.steps[s].progress_flag)
                sig = sig * 1000003u + (goblin::quest_step_done(q, s) ? (s + 1) : 0);
    }
    if (sig == built_sig_)
        return cache_;
    built_sig_ = sig;
    cache_.clear();

    GOBLIN_BENCH("build.quest_npc");
    const int gc = static_cast<int>(Category::WorldQuestNPC);
    for (size_t qi = 0; qi < QUEST_BROWSER_COUNT; qi++)
    {
        const NpcQuest &q = QUEST_BROWSER[qi];
        if (q.step_count == 0)
            continue; // placeholder entry, no steps authored yet

        // A flag-backed done step proves the quest advanced PAST every earlier step,
        // including a manual (unflagged) one sandwiched between flag-known steps (e.g.
        // Alexander's missable Gael Tunnel) or the early manual steps of an already-
        // concluded quest. So compute a floor = the last step known-done via a real
        // progress_flag and treat every step at/below it as done when picking the
        // active (first not-done) step -- else a manual gap would wrongly trap the pin
        // there, or a concluded quest would still pin step 1.
        long flag_floor = -1;
        for (size_t s = 0; s < q.step_count; s++)
            if (q.steps[s].progress_flag && goblin::quest_step_done(q, s))
                flag_floor = static_cast<long>(s);

        int done = 0;
        size_t active = q.step_count; // sentinel = "no active (not-done) step"
        for (size_t s = 0; s < q.step_count; s++)
        {
            if (goblin::quest_step_done(q, s) || static_cast<long>(s) <= flag_floor) { done++; continue; }
            if (active == q.step_count) active = s; // first not-(effectively-)done step
        }

        // questNpcQuestAware: only pin questlines already in progress (at least one
        // step done) -- matches the toggle's documented intent ("show only while the
        // quest is active"), the closest locally-computable proxy without wiring the
        // separate quest_gates.cpp flag data (open question, see
        // mapgenie_category_coverage_plan.md's cross-reference notes).
        if (goblin::config::questNpcQuestAware && done == 0)
            continue;
        if (active == q.step_count)
            continue; // every step done (or none) -> nothing to pin

        const QuestStep &step = q.steps[active];
        if (!step.entity_id)
            continue; // this step has no sourced map position yet (most steps today)

        float wx, wz;
        int grp;
        if (!entity_world_pos(step.entity_id, wx, wz, grp))
            continue; // entity not on the currently loaded map / index not built yet

        Marker m{};
        m.worldX = wx;
        m.worldZ = wz;
        m.group = grp;
        m.category = gc;
        m.color = category_color(gc);
        m.icon_key = "show_quest_npc";
        m.name_id = q.name_id ? static_cast<int>(q.name_id) : -1;
        m.source = Source::DiskMSB; // resolved from live MSB entity placement
        cache_.push_back(m);
    }
    return cache_;
}
} // namespace goblin::worldmap
