// F1 panel — "Quest navigation": the Quest Browser (hand-authored questlines with per-step
// progress, flag-backed steps, dead/hostile tints) + the runtime "Other quests" fallback.
// Moved verbatim from goblin_overlay_render.cpp::draw_panel in the split (item 1).

#include "panel_internal.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_quest_steps.hpp"        // generated::QUEST_BROWSER + QuestStep::progress_flag
#include "worldmap/quest_npc_layer.hpp"  // QuestFallbackNpc / quest_fallback_npcs

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace goblin::overlay::panel
{
void draw_quest_browser(Filter &f)
{
    if (!f.match("quest navigation browser npc questlines steps missable grey dead"))
        return;

    ImGui::SeparatorText("Quest navigation");
    ImGui::TextDisabled("Enable \"World - Quest NPC\" above to pin quest NPCs on the map.");

    // Quest Browser: ordered steps per NPC (hand-authored, original
    // text). Each step names its location/zone for manual navigation.
    // Grouped into base game + Shadow of the Erdtree via NpcQuest::dlc.
    size_t total = goblin::generated::QUEST_BROWSER_COUNT;
    size_t nbase = 0, ndlc = 0;
    for (size_t i = 0; i < total; i++)
        (goblin::generated::QUEST_BROWSER[i].dlc ? ndlc : nbase)++;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Quest Browser (%zu questlines)", total);
    if (ImGui::TreeNode(hdr))
    {
        ImGui::TextDisabled("Steps in order; location named per line.");
        ImGui::TextDisabled("Based on vanilla quests; modded profiles (ERR/Convergence/...) may differ.");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("(!) = order-sensitive / missable -- read its note before doing other quests.");
        ImGui::PopStyleColor();
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##questfilter", "filter by NPC name...",
                                 filter, sizeof(filter));
        draw_gamepad_keyboard_button("##questfilter_kbd", filter, sizeof(filter));
        // Experimental: grey out questlines whose NPC death flag is set.
        // Live (read each frame) + persisted to the ini on toggle.
        if (ImGui::Checkbox("Grey out dead-NPC questlines (experimental)",
                            goblin::overlay_api::cfg_questGreyOnDeath_ptr()))
            goblin::overlay_api::request_save();  // watcher-thread sync + persist to ini
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "POTENTIALLY BUGGY. Death flags are reverse-engineered per NPC;\n"
                "a line may grey incorrectly, or stay normal when its NPC is gone.\n"
                "Some flags are also shared with normal completion ([concluded]).\n"
                "Turn this off to always show every questline. Saved to the ini.");
        auto contains_ci = [](const char *hay, const char *need) {
            if (!need[0]) return true;
            std::string h, n;
            for (const char *p = hay; *p; ++p) h += (char)tolower(*p);
            for (const char *p = need; *p; ++p) n += (char)tolower(*p);
            return h.find(n) != std::string::npos;
        };
        // Per-step progress is keyed by NPC NAME (stable), not array
        // position: blob format "<name>=<bits>;<name>=<bits>;". So
        // reordering or inserting NPCs no longer shifts saved ticks
        // (the old positional bitstring from commit 40691d5 drifted).
        // Names carry no '=' or ';'; bits are one '0'/'1' per step.
        std::string &qp = goblin::overlay_api::cfg_questProgress_ref();
        // One-time migration: a non-empty blob with no '=' is the old
        // positional format. Walk author order and re-key by name. Base
        // NPCs precede all DLC, so their global indices are unchanged by
        // the now-authored DLC steps; legacy bits map cleanly.
        if (!qp.empty() && qp.find('=') == std::string::npos)
        {
            std::string out;
            size_t g = 0;
            for (size_t i = 0; i < total; i++)
            {
                const auto &q = goblin::generated::QUEST_BROWSER[i];
                std::string bits;
                for (size_t s = 0; s < q.step_count; s++)
                    bits += (g + s < qp.size() && qp[g + s] == '1') ? '1' : '0';
                if (bits.find('1') != std::string::npos)
                    out += std::string(q.name) + "=" + bits + ";";
                g += q.step_count;
            }
            qp = out;
        }
        // Parse keyed blob -> name -> bits.
        std::map<std::string, std::string> prog;
        for (size_t p = 0; p < qp.size();)
        {
            size_t semi = qp.find(';', p);
            if (semi == std::string::npos) semi = qp.size();
            size_t eq = qp.rfind('=', semi);
            if (eq != std::string::npos && eq > p)
                prog[qp.substr(p, eq - p)] = qp.substr(eq + 1, semi - eq - 1);
            p = semi + 1;
        }
        auto reserialize = [&]() {
            std::string out;
            for (auto &kv : prog)
                if (kv.second.find('1') != std::string::npos)
                    out += kv.first + "=" + kv.second + ";";
            qp = out;
        };
        // Flag-backed steps (QuestStep::progress_flag != 0) read straight from
        // the live EMEVD flag -- the manual ini bit is ignored for that step
        // (flag wins; see goblin_quest_steps.hpp). Flag-less steps (the common
        // case today -- per-step flags aren't sourced for most questlines yet,
        // see feat_quests Phase 2) keep the existing manual ini-blob behavior.
        auto qp_get = [&](const goblin::generated::NpcQuest &q, size_t s) {
            uint32_t flag = q.steps[s].progress_flag;
            if (flag) return goblin::overlay_api::read_event_flag(flag);
            auto it = prog.find(q.name);
            return it != prog.end() && s < it->second.size() && it->second[s] == '1';
        };
        auto qp_set = [&](const goblin::generated::NpcQuest &q, size_t s, bool v) {
            uint32_t flag = q.steps[s].progress_flag;
            if (flag)
            {
                // Read-only mirror unless the user explicitly opted into the
                // write cheat -- writing EMEVD flags can soft-lock a questline
                // or skip a reward (see config::questAllowFlagWrite's tooltip).
                if ((*goblin::overlay_api::cfg_questAllowFlagWrite_ptr()))
                    goblin::overlay_api::markers_set_event_flag(flag, v ? 1 : 0);
                return;
            }
            std::string &bits = prog[q.name];
            if (bits.size() <= s) bits.resize(s + 1, '0');
            bits[s] = v ? '1' : '0';
            reserialize();
        };
        // Render one NPC subtree. The tree ID is derived from the
        // stable array index (ptr-id overload), NOT the label text —
        // the label carries the live (done/total) count, and hashing
        // that would change the node's ID every tick and silently
        // collapse the subtree on each click.
        auto draw_npc = [&](const goblin::generated::NpcQuest &q, int id) {
            if (!contains_ci(q.name, filter)) return;
            int done = 0;
            for (size_t s = 0; s < q.step_count; s++)
                if (qp_get(q, s)) done++;
            // Visual state: grey "[unfinishable]" (NPC dead, fail_flag
            // set) takes precedence over amber "(!)" (order-sensitive /
            // missable). Both push a text tint over the whole subtree.
            bool dead = (*goblin::overlay_api::cfg_questGreyOnDeath_ptr())
                        && goblin::overlay_api::quest_unfinishable((size_t)id);
            // `concl`: the fail_flag is the NPC's shared "concluded"
            // flag (set on completion OR death) -- grey it, but label
            // it [concluded] rather than asserting the NPC is dead.
            bool concl = dead && q.fail_conclusion;
            bool warn = q.warning && q.warning[0];
            // Hostility (the "Ranni" effect): attacking this NPC sets a
            // faction-hostility flag -- they vanish until absolution. Doesn't
            // override dead/warn (those already win the tint), just adds its
            // own header tag + note so the player knows why pins disappeared.
            bool hostile = q.hostility_flag && goblin::overlay_api::read_event_flag(q.hostility_flag);
            bool tint = dead || warn || hostile;
            if (tint)
                ImGui::PushStyleColor(ImGuiCol_Text,
                    dead ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                         : ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
            bool open = ImGui::TreeNode((void *)(intptr_t)id, "%s  (%d/%zu)%s%s",
                                        q.name, done, q.step_count,
                                        dead ? (concl ? "  [concluded]"
                                                      : "  [unfinishable]")
                                             : warn ? "  (!)" : "",
                                        hostile ? "  [Hostile]" : "");
            if (tint)
                ImGui::PopStyleColor();
            if (open)
            {
                if (dead && concl)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
                    ImGui::TextWrapped("[concluded] This questline is over -- the NPC has "
                                       "either finished their story or is gone. (This flag "
                                       "is set on completion as well as on death.)");
                    ImGui::PopStyleColor();
                }
                else if (dead)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.45f, 0.45f, 1.0f));
                    ImGui::TextWrapped("[unfinishable] This questline's NPC is dead "
                                       "-- it can no longer be completed.");
                    ImGui::PopStyleColor();
                }
                if (warn)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.66f, 0.28f, 1.0f));
                    ImGui::TextWrapped("(!) %s", q.warning);
                    ImGui::PopStyleColor();
                }
                if (hostile)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.66f, 0.28f, 1.0f));
                    ImGui::TextWrapped("[Hostile -- obtain absolution at the Church of Vows "
                                       "to restore this NPC.]");
                    ImGui::PopStyleColor();
                }
                if (q.related)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                    ImGui::TextWrapped("Link: %s", q.related);
                    ImGui::PopStyleColor();
                }
                for (size_t s = 0; s < q.step_count; s++)
                {
                    ImGui::PushID((int)s);
                    bool d = qp_get(q, s);
                    bool flag_backed = q.steps[s].progress_flag != 0;
                    bool editable = !flag_backed || (*goblin::overlay_api::cfg_questAllowFlagWrite_ptr());
                    if (!editable) ImGui::BeginDisabled();
                    if (ImGui::Checkbox("##done", &d) && editable)
                        qp_set(q, s, d);
                    if (!editable) ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (flag_backed)
                    {
                        ImGui::TextDisabled("[auto]");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip((*goblin::overlay_api::cfg_questAllowFlagWrite_ptr())
                                ? "Mirrors + can write the live EMEVD flag (cheat ON) -- can break this questline."
                                : "Read-only mirror of the live EMEVD flag. Enable 'Allow writing quest flags' to edit.");
                        ImGui::SameLine();
                    }
                    ImGui::TextWrapped("%zu. %s", s + 1, q.steps[s].title);
                    ImGui::Indent();
                    if (q.steps[s].desc && q.steps[s].desc[0])
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                        ImGui::TextWrapped("%s", q.steps[s].desc);
                        ImGui::PopStyleColor();
                    }
                    if (q.steps[s].zone && q.steps[s].zone[0])
                        ImGui::TextDisabled("[%s]", q.steps[s].zone);
                    ImGui::Unindent();
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        };

        ImGui::BeginChild("questlist", ImVec2(0, 300), true);
        char gh[48];
        snprintf(gh, sizeof(gh), "Base game (%zu)", nbase);
        if (ImGui::TreeNodeEx(gh, ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (size_t i = 0; i < total; i++)
                if (!goblin::generated::QUEST_BROWSER[i].dlc)
                    draw_npc(goblin::generated::QUEST_BROWSER[i], (int)i);
            ImGui::TreePop();
        }
        snprintf(gh, sizeof(gh), "Shadow of the Erdtree (%zu)", ndlc);
        if (ImGui::TreeNodeEx(gh, ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (size_t i = 0; i < total; i++)
                if (goblin::generated::QUEST_BROWSER[i].dlc)
                    draw_npc(goblin::generated::QUEST_BROWSER[i], (int)i);
            ImGui::TreePop();
        }
        // Runtime FALLBACK: quest NPCs the active mod's EMEVD exposes (load_quest_npcs)
        // not covered by a hand entry above — modded / not-yet-authored. The disk worker
        // gives raw candidates (concluded/register/npcParamId, NO name); resolve the NAME
        // (npc_team_and_name reads live NpcParam → must be here, not the disk worker) +
        // the secondary name-coverage HERE, cached (candidate set is set once). Minimal
        // view: name + live [concluded] state, no step prose (mod-agnostic fallback).
        {
            std::vector<goblin::worldmap::QuestFallbackNpc> cand =
                goblin::worldmap::quest_fallback_npcs();
            static std::vector<goblin::worldmap::QuestFallbackNpc> s_qfb;
            static size_t s_qfb_gen = ~size_t{0};
            if (cand.size() != s_qfb_gen)
            {
                s_qfb_gen = cand.size();
                s_qfb.clear();
                // Hand coverage keys. name_id (FMG id) is LANGUAGE-INDEPENDENT — the
                // reliable join. Names (English) only weakly match a same-language FMG,
                // so keep them as a secondary signal only.
                std::unordered_set<int32_t> handNameIds;
                std::vector<std::string> handNames;
                for (size_t i = 0; i < goblin::generated::QUEST_BROWSER_COUNT; i++)
                {
                    if (goblin::generated::QUEST_BROWSER[i].name_id)
                        handNameIds.insert((int32_t)goblin::generated::QUEST_BROWSER[i].name_id);
                    if (goblin::generated::QUEST_BROWSER[i].name)
                    {
                        std::string s = goblin::generated::QUEST_BROWSER[i].name;
                        for (char &c : s) c = (char)tolower((unsigned char)c);
                        handNames.push_back(std::move(s));
                    }
                }
                // Pass 1: resolve each candidate's (name, nameId); count nameId frequency
                // so GENERIC NPCs (merchants/mobs sharing one NpcName across many flags —
                // "Nomadic Merchant" ×N) drop out language-independently.
                std::vector<std::pair<std::string, int32_t>> resolved(cand.size());
                std::unordered_map<std::string, int> freq;  // by TEXT: merchants share distinct nameIds but identical text
                for (size_t i = 0; i < cand.size(); i++)
                {
                    if (cand[i].handCovered) continue;  // pinned on the map, but not an "Other quest"
                    for (uint32_t param : cand[i].npcParamIds)
                    {
                        uint8_t team = 0;
                        int32_t nameId = 0;
                        if (goblin::overlay_api::npc_team_and_name(param, &team, &nameId) && nameId > 0)
                        {
                            std::string nm = goblin::overlay_api::lookup_text_utf8(nameId + 700000000);
                            if (!nm.empty()) { freq[nm]++; resolved[i] = {std::move(nm), nameId}; break; }
                        }
                    }
                }
                // Pass 2: keep only named, non-generic, hand-uncovered candidates.
                for (size_t i = 0; i < cand.size(); i++)
                {
                    if (cand[i].handCovered) continue;  // pinned on the map, but not an "Other quest"
                    const std::string &nm = resolved[i].first;
                    int32_t nameId = resolved[i].second;
                    if (nm.empty()) continue;
                    if (freq[nm] > 1) continue;               // generic (merchant/mob — same name on many flags)
                    if (handNameIds.count(nameId)) continue;  // covered by a hand entry (language-independent)
                    std::string ln = nm;
                    for (char &ch : ln) ch = (char)tolower((unsigned char)ch);
                    bool cov = false;
                    for (const std::string &hn : handNames)
                        if (hn.size() >= 4 && (ln.find(hn) != std::string::npos || hn.find(ln) != std::string::npos))
                        { cov = true; break; }
                    if (cov) continue;
                    cand[i].name = nm;
                    s_qfb.push_back(cand[i]);
                }
                if (!cand.empty())
                    spdlog::info("[QUESTNPC] browser fallback: {} candidates -> {} shown "
                                 "(hand-covered by {} name_ids + fail_flags)",
                                 cand.size(), s_qfb.size(), (int)handNameIds.size());
            }
            if (!s_qfb.empty())
            {
                snprintf(gh, sizeof(gh), "Other quests \xE2\x80\x94 auto-detected (%zu)", s_qfb.size());
                if (ImGui::TreeNodeEx(gh))
                {
                    ImGui::TextDisabled("Found in this mod's data; no step guide yet.");
                    for (const auto &n : s_qfb)
                    {
                        bool done = goblin::overlay_api::read_event_flag(n.concluded);
                        ImGui::BulletText("%s  %s", n.name.c_str(),
                                          done ? "[concluded]" : "[in progress]");
                    }
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Tick steps to track progress; Save to keep it. Original text.");
        ImGui::TreePop();
    }
}
} // namespace goblin::overlay::panel
