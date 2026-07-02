// F1 panel — "Debug" (live projection toggle), "Dev tools (Save + restart)" (observer
// hooks + the NPC death-flag capture tool) and the "Danger zone" (confirm-gated resets).
// Moved verbatim from goblin_overlay_render.cpp::draw_panel in the split (item 1).

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_quest_steps.hpp"  // generated::QUEST_BROWSER (capture NPC combo)

namespace goblin::overlay::panel
{
using goblin::i18n::tr;  // overlay UI localization (lang/<code>.txt)

void draw_dev_tools_danger(Filter &f)
{
    // Dev/debug observers. Each installs a hook or starts a worker thread
    // ONCE at startup based on its config flag (dllmain setup), so these
    // are restart-required: flip + Save, then relaunch. save_all_bool_settings
    // persists every config bool, so no per-flag plumbing is needed.
    if (f.match("debug live projection engine world map dungeon underground placement"))
    {
        ImGui::SeparatorText(tr("Debug"));
        // Live toggle (no restart): project_marker reads this every frame.
        ImGui::Checkbox(tr("Live projection (engine world→map fn; fixes dungeon/UG placement)"),
                        goblin::overlay_api::cfg_liveProjection_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Project markers with the game's OWN WorldMapViewModel instead of the baked\n"
                "affine. Toggles live (open the map and flip it to compare). Underground /\n"
                "dungeon markers snap to their real LegacyConv-folded positions. Falls back\n"
                "to baked until the map is open (engine VM must resolve).");
    }
    const bool show_devtools = f.match("dev tools save restart event-flag item-grant hook "
                                       "cursor probe marker dump hotkey verbose logging "
                                       "flag capture npc death");
    if (f.filtering && show_devtools) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_devtools && ImGui::TreeNode("Dev tools (Save + restart)"))
    {
        ImGui::Checkbox("Event-flag hook (coverage-gap detector)",
                        goblin::overlay_api::cfg_debugEventFlags_ptr());
        ImGui::Checkbox("Item-grant hook (coverage-gap detector)",
                        goblin::overlay_api::cfg_debugItemGrants_ptr());
        ImGui::Checkbox("World-map cursor probe (logs cursor coords)",
                        goblin::overlay_api::cfg_debugWorldmapProbe_ptr());
        ImGui::Checkbox("Marker-dump hotkey (F9 → markers log)",
                        goblin::overlay_api::cfg_enableMarkerDump_ptr());
        ImGui::Checkbox("Verbose logging (addresses, param internals)",
                        goblin::overlay_api::cfg_debugLogging_ptr());
        ImGui::Checkbox("Flag-capture hook (NPC death-flag tool)",
                        goblin::overlay_api::cfg_debugFlagCapture_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("These take effect after Save + a game restart — each\n"
                              "installs its hook/worker once at startup.");

        // Flag-capture tool: arm naming an NPC, kill it, finalize -> the
        // persisted death flag(s) are logged as fail_flag candidates.
        ImGui::Separator();
        ImGui::TextDisabled("NPC death-flag capture (needs the hook above + restart)");
        static int cap_sel = 0;
        size_t qn = goblin::generated::QUEST_BROWSER_COUNT;
        if (cap_sel >= (int)qn) cap_sel = 0;
        const char *cap_name = goblin::generated::QUEST_BROWSER[cap_sel].name;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##capnpc", cap_name))
        {
            for (int i = 0; i < (int)qn; i++)
                if (ImGui::Selectable(goblin::generated::QUEST_BROWSER[i].name,
                                      i == cap_sel))
                    cap_sel = i;
            ImGui::EndCombo();
        }
        static int cap_last = -1;
        if (!goblin::overlay_api::debug_events_capture_armed())
        {
            if (ImGui::Button("Arm capture"))
            {
                goblin::overlay_api::debug_events_arm_capture(cap_name);
                cap_last = -1;
            }
            if (cap_last >= 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                   "logged %d -> flagcapture.txt", cap_last);
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
            ImGui::TextWrapped("ARMED for '%s' — kill it, wait ~5s, then Finalize.",
                               cap_name);
            ImGui::PopStyleColor();
            ImGui::Text("flags captured: %zu", goblin::overlay_api::debug_events_capture_count());
            if (ImGui::Button("Finalize -> log"))
                cap_last = goblin::overlay_api::debug_events_finalize_capture(
                    &goblin::overlay_api::read_event_flag);
        }
        ImGui::TreePop();
    }

    // ── Danger zone: destructive resets behind a confirm popup ────────
    // Push/Pop stay unconditional so the pop below never unbalances; only the
    // separator + node hide while the settings search filters this out.
    const bool show_danger = f.match("danger zone reset quest progression parameters defaults");
    if (show_danger)
        ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.13f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.22f, 0.22f, 1.0f));
    if (f.filtering && show_danger) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_danger && ImGui::TreeNode(tr("Danger zone")))
    {
        if (ImGui::Button(tr("Reset quest progression")))
            ImGui::OpenPopup("##confirm_reset_quest");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Clear every quest-step checkmark. Save to INI to persist."));
        if (ImGui::Button(tr("Reset parameters to default")))
            ImGui::OpenPopup("##confirm_reset_params");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Restore all settings to defaults and write the ini.\n"
                                       "Restart the game to fully apply."));

        if (ImGui::BeginPopupModal("##confirm_reset_quest", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(tr("Clear ALL quest-step checkmarks?"));
            ImGui::TextDisabled("%s", tr("Cannot be undone. Save to INI afterwards to persist."));
            if (ImGui::Button(tr("Yes, clear")))
            {
                goblin::overlay_api::reset_quest_progress();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel")))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("##confirm_reset_params", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(tr("Reset ALL settings to defaults?"));
            ImGui::TextDisabled("%s", tr("Writes defaults to MapForGoblins.ini. Restart to fully apply."));
            if (ImGui::Button(tr("Yes, reset")))
            {
                goblin::overlay_api::reset_to_defaults();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel")))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::TreePop();
    }
    ImGui::PopStyleColor(3);
}
} // namespace goblin::overlay::panel
