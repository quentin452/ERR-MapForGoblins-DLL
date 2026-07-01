#include "quest_npc_layer.hpp"

#include "category_meta.hpp"      // category_color
#include "map_entry_layer.hpp"    // entity_world_pos
#include "goblin_map_data.hpp"    // goblin::generated::Category
#include "goblin_inject.hpp"      // goblin::ui::category_visible/section_visible/category_section
#include "goblin_quest_steps.hpp" // goblin::generated::QUEST_BROWSER, goblin::quest_step_done
#include "goblin_config.hpp"      // config::questProgress
#include "goblin_bench.hpp"       // GOBLIN_BENCH scoped timers

#include <functional>
#include <mutex>
#include <unordered_set>

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
    // Runtime EMEVD-sourced quest NPCs (mod-agnostic, load_quest_npcs → disk worker). Fetched
    // ONCE here: folded into the signature below and reused by the build loop.
    const std::vector<QuestFallbackNpc> fb = quest_fallback_npcs();

    size_t sig = std::hash<std::string>{}(goblin::config::questProgress);
    for (size_t qi = 0; qi < QUEST_BROWSER_COUNT; qi++)
    {
        const NpcQuest &q = QUEST_BROWSER[qi];
        for (size_t s = 0; s < q.step_count; s++)
            if (q.steps[s].progress_flag)
                sig = sig * 1000003u + (goblin::quest_step_done(q, s) ? (s + 1) : 0);
    }
    // Fallback set: its content (concluded ids) + how many currently resolve to a world position.
    // The resolve-count changes 0→N once the disk entity index is built, so the pins appear on the
    // first build after that (the [concluded] tooltip state stays LIVE at hover, so it's excluded
    // from the signature -- it must not force a rebuild).
    sig = sig * 1000003u + fb.size();
    size_t fbResolved = 0;
    for (const QuestFallbackNpc &n : fb)
    {
        sig = sig * 1000003u + n.concluded;
        float x, z; int g;
        if (n.pinEntity && entity_world_pos(n.pinEntity, x, z, g))
            fbResolved++;
    }
    sig = sig * 1000003u + fbResolved;
    if (sig == built_sig_)
        return cache_;
    built_sig_ = sig;
    cache_.clear();

    GOBLIN_BENCH("build.quest_npc");
    const int gc = static_cast<int>(Category::WorldQuestNPC);
    // name_ids of hand entries that produced a step-following pin below. The runtime fallback
    // loop skips these so an NPC with hand step data (Boc/Alexander/Thops) isn't ALSO static-pinned.
    std::unordered_set<int32_t> handPinnedName;
    // fail_flags the hand table VETTED as a true "dead/gone" flag (fail_conclusion==false → NOT a
    // shared merchant/completion flag). ONLY these get a live [concluded]/[in progress] on a fallback
    // pin. Every other runtime flag is unvetted — its raw _q99 can't tell "quest done" from "merchant
    // available / relocated" (e.g. Kalé) — so it shows a neutral "optional" tag instead of a
    // misleading state. Keyed by flag value, so it's language-independent.
    std::unordered_set<uint32_t> vettedFail;
    for (size_t qi = 0; qi < QUEST_BROWSER_COUNT; qi++)
        if (QUEST_BROWSER[qi].fail_flag && !QUEST_BROWSER[qi].fail_conclusion)
            vettedFail.insert(QUEST_BROWSER[qi].fail_flag);
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

        size_t active = q.step_count; // sentinel = "no active (not-done) step"
        for (size_t s = 0; s < q.step_count; s++)
        {
            if (goblin::quest_step_done(q, s) || static_cast<long>(s) <= flag_floor) continue;
            if (active == q.step_count) { active = s; break; } // first not-(effectively-)done step
        }

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
        // name_id must be the FMG-ROUTED id for the tooltip/search (lookup_text routes
        // NpcName at +700000000 — same convention as every other marker; the raw hand
        // name_id gave an empty tooltip).
        m.name_id = q.name_id ? static_cast<int>(q.name_id + 700000000u) : -1;
        m.tip_quest = q.quest_title;       // "Boc's Quest"
        m.tip_step = step.title;           // active step title = the current "palier"
        m.tip_zone = step.zone;            // coarse location ("Limgrave")
        m.source = Source::DiskMSB; // resolved from live MSB entity placement
        cache_.push_back(m);
        if (q.name_id)
            handPinnedName.insert(static_cast<int32_t>(q.name_id));
    }

    // Runtime fallback pins: EVERY quest NPC the active mod's EMEVD exposes (mod-agnostic), pinned
    // STATICALLY at its first placement. The 3 with hand step data are already step-pinned above
    // (skipped here by name_id). Name + [concluded]/[in progress] state are resolved LIVE (NpcParam
    // + event flag), so this is correct for any mod -- no hand authoring, no bake.
    for (const QuestFallbackNpc &n : fb)
    {
        if (!n.pinEntity)
            continue;
        float wx, wz;
        int grp;
        if (!entity_world_pos(n.pinEntity, wx, wz, grp))
            continue; // entity not on the currently loaded map / index not built yet

        // Localized name from any of the NPC's placements (first with a real NpcName; a quest NPC's
        // first placement is often a logic param with nameId 0 -- try them all).
        int32_t nameId = 0;
        for (uint32_t param : n.npcParamIds)
        {
            uint8_t team = 0;
            int32_t nid = 0;
            if (goblin::npc_team_and_name(param, &team, &nid) && nid > 0) { nameId = nid; break; }
        }
        if (nameId && handPinnedName.count(nameId))
            continue; // this NPC already has a richer step-following pin above

        Marker m{};
        m.worldX = wx;
        m.worldZ = wz;
        m.group = grp;
        m.category = gc;
        m.color = category_color(gc);
        m.icon_key = "show_quest_npc";
        // FMG-routed id (+700000000) for the tooltip name, same convention as the hand pin.
        m.name_id = nameId ? static_cast<int>(nameId + 700000000u) : -1;
        m.tip_quest = "Auto-detected quest"; // marks it a quest pin for marker_label (no step prose)
        if (vettedFail.count(n.concluded))
            m.quest_concluded_flag = static_cast<int>(n.concluded); // vetted flag → live [concluded]/[in progress]
        else
            // Unvetted runtime flag (merchant/optional like Kalé): can't prove completion → neutral tag.
            m.tip_step = "optional";
        m.source = Source::DiskMSB;
        cache_.push_back(m);
    }
    return cache_;
}
} // namespace goblin::worldmap
