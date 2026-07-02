// F1 panel — "Clustering": master enable, cluster size, distance-adaptive knobs + debug
// viz toggles, and the player-profile presets. Moved verbatim from
// goblin_overlay_render.cpp::draw_panel in the split (big-files refactor item 1).

#include "panel_internal.hpp"
#include "goblin_overlay_render_api.hpp"

namespace goblin::overlay::panel
{
void draw_clustering(Filter &f)
{
    if (!f.match("clustering cluster enable declutter dense size threshold distance "
                 "adaptive near far radius preset completionist explorer performance"))
        return;

    ImGui::SeparatorText("Clustering");
    // Master enable — ALWAYS shown. (Bug: this was gated on
    // clustering_active(), so once a re-plan found no pile over the
    // threshold the whole block vanished and clustering could not be
    // re-enabled without a restart.) Drives config::enableClustering and
    // re-plans live; enabled ⇔ dense piles collapsed into one icon.
    bool en = goblin::overlay_api::clustering_enabled();
    if (ImGui::Checkbox("Enable clustering (declutter dense areas)", &en))
        goblin::overlay_api::set_clustering_enabled(en);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Collapse dense marker piles into one icon to keep busy\n"
                          "regions readable. NOT for performance — the overlay map\n"
                          "has no freeze. Live (updates as you pan/zoom the map).");

    if (en)
    {
        ImGui::TextDisabled("One mixed pile per location. Per-category opt-in above.");
        // Cluster size = how many markers a LOCATION must hold to collapse
        // into one pile (sparse locations stay normal).
        int gt = goblin::overlay_api::global_threshold();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("Cluster size — markers per location (live)", &gt))
            goblin::overlay_api::set_global_threshold(gt);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A location collapses into one pile when it holds MORE\n"
                              "than this many (clustered-category) markers. LOWER = MORE\n"
                              "clustering. Min 1. Live — reopen the map to see it.\n"
                              "When distance-adaptive is on, this is the FAR (clustered) size.");

        // Distance-adaptive: scale the size up far from the player.
        bool da = (*goblin::overlay_api::cfg_clusterDistanceAdaptive_ptr());
        if (ImGui::Checkbox("Distance-adaptive (cluster by distance from player)", &da))
        {
            (*goblin::overlay_api::cfg_clusterDistanceAdaptive_ptr()) = da;
            goblin::overlay_api::request_cluster_replan();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Own mode: OVERRIDES the per-category opt-in above and clusters\n"
                              "EVERY category by distance — full detail (individual items)\n"
                              "near you, dense spots far away merged into piles. Ramps from\n"
                              "the near size to the far size below. OVERWORLD ONLY (the\n"
                              "underground player position isn't available, so underground\n"
                              "uses normal threshold + per-category). Live.");
        if (da)
        {
            int nr = (*goblin::overlay_api::cfg_clusterNearRadius_ptr());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Near radius (tiles)", &nr, 1, 40))
            { (*goblin::overlay_api::cfg_clusterNearRadius_ptr()) = static_cast<uint8_t>(nr); goblin::overlay_api::request_cluster_replan(); }
            int nt = (*goblin::overlay_api::cfg_clusterNearThreshold_ptr());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Detail near you (cluster size)", &nt, 1, 60))
            { (*goblin::overlay_api::cfg_clusterNearThreshold_ptr()) = static_cast<uint8_t>(nt); goblin::overlay_api::request_cluster_replan(); }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cluster size WITHIN the near radius (higher = more individual\n"
                                  "items shown near you). Ramps down to 'Cluster size' (above) far away.");
            int fr = (*goblin::overlay_api::cfg_clusterFarRadius_ptr());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Far radius (tiles)", &fr, 2, 80))
            { (*goblin::overlay_api::cfg_clusterFarRadius_ptr()) = static_cast<uint8_t>(fr); goblin::overlay_api::request_cluster_replan(); }

            // Debug viz: draw the player + near/far rings (overworld) and
            // per-pile sub-page tab (underground) so you can SEE where the
            // distance ramp engages and pin the bug.
            ImGui::Checkbox("DEBUG: distance zones (player + near/far rings)",
                            goblin::overlay_api::cfg_clusterDebugRadius_ptr());
            ImGui::Checkbox("DEBUG: marker projection/tile (green=live red=baked + tile)",
                            goblin::overlay_api::cfg_clusterDebugMarkers_ptr());
            ImGui::Checkbox("DEBUG: cluster anchors (pile→member lines + name)",
                            goblin::overlay_api::cfg_debugClusterAnchors_ptr());
            ImGui::Checkbox("DEBUG: region volumes (names; red = unresolved)",
                            goblin::overlay_api::cfg_debugRegionVolumes_ptr());
        }

        // Player-profile presets — one click sets every cluster knob.
        ImGui::TextDisabled("Preset:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Completionist"))
        {
            // Anchors aggregate ~40 markers each, so only a very high
            // threshold leaves them as individual items everywhere.
            goblin::overlay_api::set_global_threshold(60);
            (*goblin::overlay_api::cfg_clusterDistanceAdaptive_ptr()) = false;
            goblin::overlay_api::request_cluster_replan();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max detail: individual items everywhere; only the densest spots cluster. No distance scaling.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Explorer"))
        {
            // Tight radius: only the area immediately around you is detailed,
            // everything else clusters (covers just the useful part of the
            // map, not the whole thing). Verified-good config.
            goblin::overlay_api::set_global_threshold(4);           // far = clustered
            (*goblin::overlay_api::cfg_clusterDistanceAdaptive_ptr()) = true;
            (*goblin::overlay_api::cfg_clusterNearThreshold_ptr()) = 60;     // near = individual items
            (*goblin::overlay_api::cfg_clusterNearRadius_ptr()) = 1;
            (*goblin::overlay_api::cfg_clusterFarRadius_ptr()) = 2;
            goblin::overlay_api::request_cluster_replan();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Balanced: individual items in your immediate area, everything else merged.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Performance"))
        {
            goblin::overlay_api::set_global_threshold(2);           // far = very aggressive
            (*goblin::overlay_api::cfg_clusterDistanceAdaptive_ptr()) = true;
            (*goblin::overlay_api::cfg_clusterNearThreshold_ptr()) = 30;     // tighter detail bubble
            (*goblin::overlay_api::cfg_clusterNearRadius_ptr()) = 1;
            (*goblin::overlay_api::cfg_clusterFarRadius_ptr()) = 2;
            goblin::overlay_api::request_cluster_replan();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aggressive far-merge: fewest distant icons, most declutter.");
    }
}
} // namespace goblin::overlay::panel
