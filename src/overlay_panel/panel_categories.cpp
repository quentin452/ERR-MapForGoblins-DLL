// F1 panel — "Sections & categories" (the per-section/per-category visibility + cluster
// grid with its search box) and the "ERR integration" block. Moved verbatim from
// goblin_overlay_render.cpp::draw_panel in the split (big-files refactor item 1).

#include "panel_internal.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_map_data.hpp"  // generated::Category (landmark suppression row range)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace goblin::overlay::panel
{
void draw_sections_categories(Filter &f)
{
    // Sections (coarse) + their categories (fine). A row shows only if
    // both its section and its category are enabled.
    static char cat_filter[64] = "";
    const bool show_seccat = f.match("sections categories marker show hide cluster world");
    if (show_seccat)
    {
        ImGui::SeparatorText("Sections & categories");
        // Search box: filter the category list by name. Matching a SECTION name
        // shows that whole section; otherwise only matching category rows show,
        // and sections with no match are hidden. Sections auto-expand while
        // filtering so matches are visible without manual clicking.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##catfilter", "search categories... (e.g. sorcer, ash, smith)",
                                 cat_filter, sizeof(cat_filter));
        draw_gamepad_keyboard_button("##catfilter_kbd", cat_filter, sizeof(cat_filter));
    }
    const bool cat_filtering = cat_filter[0] != '\0';
    for (int s = 0; show_seccat && s < goblin::overlay_api::section_count(); s++)
    {
        bool sec_name_match = contains_ci(goblin::overlay_api::section_label(s), cat_filter);
        if (cat_filtering && !sec_name_match)
        {
            // Skip a section entirely if neither it nor any of its categories match.
            bool any = false;
            for (int c = 0; c < goblin::overlay_api::category_count() && !any; c++)
                if (goblin::overlay_api::category_section(c) == s &&
                    contains_ci(goblin::overlay_api::category_label(c), cat_filter))
                    any = true;
            if (!any) continue;
        }
        if (cat_filtering)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (!ImGui::TreeNode(goblin::overlay_api::section_label(s)))
            continue;
        // While filtering, a category row shows only if it matches (unless the
        // section name itself matched → show the whole section).
        const bool show_all_cats = !cat_filtering || sec_name_match;

        bool sv = goblin::overlay_api::section_visible(s);
        if (ImGui::Checkbox("(whole section)", &sv))
            goblin::overlay_api::set_section_visible(s, sv);

        ImGui::PushID(s);
        if (ImGui::SmallButton("Show all"))
            for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                if (goblin::overlay_api::category_section(c) == s)
                    goblin::overlay_api::set_category_visible(c, true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show none"))
            for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                if (goblin::overlay_api::category_section(c) == s)
                    goblin::overlay_api::set_category_visible(c, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cluster all"))
            for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                if (goblin::overlay_api::category_section(c) == s)
                    goblin::overlay_api::set_category_clustered(c, true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cluster none"))
            for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                if (goblin::overlay_api::category_section(c) == s)
                    goblin::overlay_api::set_category_clustered(c, false);
        ImGui::PopID();
        ImGui::TextDisabled("left = show on map   |   right [cluster] = join location pile (live) / unchecked = shown normally");
        ImGui::Separator();

        // Rows sorted ALPHABETICALLY by label (user request 2026-07-02): the enum order
        // appends every new category at the end of its section, which reads as random.
        // Sorted once (labels are static for the session).
        static std::vector<int> s_cat_order;
        if (s_cat_order.empty())
        {
            for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                s_cat_order.push_back(c);
            std::sort(s_cat_order.begin(), s_cat_order.end(), [](int a, int b) {
                return std::strcmp(goblin::overlay_api::category_label(a),
                                   goblin::overlay_api::category_label(b)) < 0;
            });
        }
        for (int c : s_cat_order)
        {
            if (goblin::overlay_api::category_section(c) != s) continue;
            if (!show_all_cats && !contains_ci(goblin::overlay_api::category_label(c), cat_filter))
                continue;   // search box: hide non-matching category rows
            ImGui::PushID(c);
            const char *clabel = goblin::overlay_api::category_label(c);
            bool cv = goblin::overlay_api::category_visible(c);
            if (ImGui::Checkbox(clabel, &cv))
                goblin::overlay_api::set_category_visible(c, cv);
            // Landmark categories drive the native-pin suppression (a WMPP areaNo flip
            // the game only re-reads when it rebuilds its pin list) → mark the rows the
            // "Hide native landmark pins" option affects, with the map-reopen caveat.
            if ((*goblin::overlay_api::cfg_landmarkSuppressNative_ptr()) &&
                c >= static_cast<int>(goblin::generated::Category::WorldDivineTower) &&
                c <= static_cast<int>(goblin::generated::Category::WorldUniqueSite))
            {
                ImGui::SameLine();
                ImGui::TextDisabled("↻");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Also hides the game's OWN pin for these spots while enabled\n"
                                      "(\"Hide native landmark pins\" in Settings, avoids duplicates).\n"
                                      "The native pin change shows the NEXT time the map is opened.");
            }
            // Capture row width once, before any SameLine, so the badge and
            // the cluster checkbox both position from a stable origin.
            float row_avail = ImGui::GetContentRegionAvail().x;
            // Uncollected badge: "<remaining>/<total>" of collectible items in
            // this category. Skipped for categories with no collectible rows
            // (graces/NPCs/regions → total 0). Green once fully looted.
            int rem = goblin::overlay_api::category_remaining(c);
            int tot = goblin::overlay_api::category_total(c);
            if (tot > 0)
            {
                char cntbuf[24];
                snprintf(cntbuf, sizeof(cntbuf), "%d/%d", rem < 0 ? 0 : rem, tot);
                ImGui::SameLine(row_avail - 150.0f);
                ImVec4 col = (rem == 0) ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)   // all collected
                                        : ImVec4(0.85f, 0.82f, 0.45f, 1.0f);  // some left
                ImGui::TextColored(col, "%s", cntbuf);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Items not yet collected / total collectible in this category.");
            }
            // Right-aligned cluster opt-in: checked = this category's markers
            // join the location pile; unchecked = shown normally on the map.
            // Live (re-plans on next map open).
            ImGui::SameLine(row_avail - 70.0f);
            bool clu = goblin::overlay_api::category_clustered(c);
            if (ImGui::Checkbox("cluster", &clu))
                goblin::overlay_api::set_category_clustered(c, clu);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Checked = this category's markers fold into the\n"
                                  "one-per-location cluster pile. Unchecked = shown\n"
                                  "normally on the map. Live — reopen the map to apply.");
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    if (f.match("err integration reforged hide boss markers camps completion"))
    {
        ImGui::SeparatorText("ERR integration");
        bool hide_bosses = goblin::overlay_api::err_hide_bosses();
        if (ImGui::Checkbox("Hide boss markers (ERR already marks bosses)", &hide_bosses))
            goblin::overlay_api::set_err_hide_bosses(hide_bosses);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ELDEN RING Reforged natively marks bosses (and enemy camps,\n"
                              "plus completion markers) on the world map. Enable this to hide\n"
                              "MapForGoblins' own boss markers and avoid the duplicate.\n"
                              "Same as unchecking 'World - Bosses' above; persists on Save.");
        ImGui::TextDisabled("ERR also marks enemy camps & completion (cleared dungeons/ruins).");
    }
}
} // namespace goblin::overlay::panel
