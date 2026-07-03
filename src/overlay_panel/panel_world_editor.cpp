// F1 panel — "World Editor (live)": the in-game world editor (runtime-modding framework vision #2).
// Pick an AEG asset, see the loot item its MAP MARKER resolves to (the same live chain the map build
// uses), then either RE-SKIN that lot's slot-1 item in place, or REPOINT the asset at a different
// existing lot (non-destructive — leaves the shared lot alone). Refresh the markers to see the edit
// on the map — the whole regulation-free live-edit loop as a UI. Backend proven via the
// loot_at / param_setf / refresh_markers RPCs (2026-07-03).
//
// Slice 1 (2026-07-03): live loot re-skin (pickUpItemLotParamId → lot → resolve → set lotItemId01).
// Slice 2 (2026-07-03): repoint-to-another-lot (set pickUpItemLotParamId to a different EXISTING lot,
//   with a live preview of the target lot's item). Repointing at an existing lot resolves live on
//   Refresh markers — the refresh_markers v2 LotReader-index reset is only needed for NEWLY CLONED
//   lots, which the CLONE slice will add on top of this.

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "goblin_overlay_render_api.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace goblin::overlay::panel
{
using goblin::i18n::tr;  // overlay UI localization (lang/<code>.txt)

void draw_world_editor(Filter &f)
{
    if (!f.match("world editor live loot item asset aeg marker repoint lot goods edit param mod"))
        return;

    ImGui::SeparatorText(tr("World Editor (live)"));
    ImGui::TextDisabled("%s", tr("Re-skin a loot spot: pick an asset, set its item, refresh the map."));

    static int aeg = 99036;  // an early collectible asset (Bloodrose) as a friendly default
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("Asset (aegRow)"), &aeg);

    // Resolve LIVE what this asset's marker currently shows — aeg_pickup_lot -> lot ->
    // resolve_loot_item_textid -> name. Same chain the marker build uses, so the display updates the
    // instant an edit lands (before refresh_markers even propagates to the drawn map).
    int lot = (aeg > 0) ? goblin::overlay_api::aeg_pickup_lot(aeg) : 0;
    int32_t textid = lot ? goblin::overlay_api::resolve_loot_item_textid((uint32_t)lot, 1, -1) : -1;
    std::string name = (textid >= 0) ? goblin::overlay_api::lookup_text_utf8(textid) : std::string{};
    ImGui::Text("%s %d  \xE2\x86\x92  %s", tr("Lot"), lot,
                name.empty() ? tr("(none / not a pickup asset)") : name.c_str());

    static int new_item = 8000000;  // a reserved custom-goods id (define via custom_items.toml)
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("New goods id"), &new_item);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Raw goods row id (must be <= 8388606 to be grantable).\n"
                                   "Define its name/stats first via custom_items.toml; an\n"
                                   "existing goods id works too. Edits this lot's slot-1 item\n"
                                   "IN PLACE (affects every asset pointing at this lot)."));

    static char status[160] = "";
    ImGui::BeginDisabled(lot == 0);
    if (ImGui::Button(tr("Set item on this asset's lot")))
    {
        bool ok = goblin::overlay_api::param_set_field("ItemLotParam_map", (uint64_t)lot,
                                                       "lotItemId01", (double)new_item);
        std::snprintf(status, sizeof(status),
                      ok ? "ok: lot %d lotItemId01 = %d (Refresh markers to see it)"
                         : "FAILED to set lot %d", lot, new_item);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("IN-PLACE re-skin: changes the shared lot itself, so EVERY asset\n"
                                   "that points at this lot changes too. To change only THIS asset,\n"
                                   "use Repoint below instead."));

    // ── Slice 2: repoint this asset at a DIFFERENT existing lot ──────────────────────────────────
    // Non-destructive: leaves the current (shared) lot untouched and just points this one asset's
    // pickUpItemLotParamId elsewhere. Live preview shows what the target lot yields before committing.
    ImGui::Spacing();
    static int target_lot = 0;
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("Repoint to lot"), &target_lot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Point THIS asset at a different EXISTING ItemLotParam_map row\n"
                                   "(non-destructive — the current lot is left unchanged). A newly\n"
                                   "CLONED lot won't resolve on the map until the CLONE slice lands\n"
                                   "the LotReader-index reset; existing lots reflect live."));

    // Preview the target lot's slot-1 item live, same chain as the current-lot display above.
    if (target_lot > 0)
    {
        int32_t t_textid = goblin::overlay_api::resolve_loot_item_textid((uint32_t)target_lot, 1, -1);
        std::string t_name = (t_textid >= 0) ? goblin::overlay_api::lookup_text_utf8(t_textid)
                                             : std::string{};
        ImGui::SameLine();
        ImGui::TextDisabled("\xE2\x86\x92 %s",
                            t_name.empty() ? tr("(no item / unknown lot)") : t_name.c_str());
    }

    ImGui::BeginDisabled(aeg <= 0 || target_lot <= 0);
    if (ImGui::Button(tr("Repoint asset to this lot")))
    {
        bool ok = goblin::overlay_api::param_set_field(
            "AssetEnvironmentGeometryParam", (uint64_t)aeg, "pickUpItemLotParamId", (double)target_lot);
        std::snprintf(status, sizeof(status),
                      ok ? "ok: asset %d pickUpItemLotParamId = %d (Refresh markers to see it)"
                         : "FAILED to repoint asset %d", aeg, target_lot);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button(tr("Refresh markers")))
    {
        goblin::overlay_api::rebuild_markers();
        std::snprintf(status, sizeof(status), "markers rebuilding on the disk worker — open/pan the map");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Rebuild the map markers from live params so the edit shows.\n"
                                   "Full rebuild (~2s) on a worker thread — no frame hitch."));

    if (status[0]) ImGui::TextDisabled("%s", status);
}
}  // namespace goblin::overlay::panel
