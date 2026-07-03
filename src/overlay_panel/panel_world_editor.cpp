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

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

    // Slot selector (slice 3): an ItemLotParam row has 8 item slots (lotItemId01..08). Slot 1 drives
    // the map marker, but any slot can be re-skinned. Pick a slot, see its current id live, set it.
    static int slot = 1;  // 1-based (matches the lotItemId0N field naming)
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt(tr("Slot"), &slot, 1, 8);
    if (slot < 1) slot = 1; else if (slot > 8) slot = 8;
    char field[16];
    std::snprintf(field, sizeof(field), "lotItemId%02d", slot);

    // Live-read the selected slot's current item id (0 = empty slot).
    double cur = 0.0;
    bool cur_ok = lot && goblin::overlay_api::param_get_field("ItemLotParam_map", (uint64_t)lot,
                                                              field, &cur);
    ImGui::SameLine();
    if (cur_ok)
        ImGui::TextDisabled("%s = %d", field, (int)cur);
    else
        ImGui::TextDisabled("%s", tr("(no lot)"));

    static int new_item = 8000000;  // a reserved custom-goods id (define via custom_items.toml)
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("New goods id"), &new_item);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Raw goods row id (must be <= 8388606 to be grantable).\n"
                                   "Define its name/stats first via custom_items.toml; an\n"
                                   "existing goods id works too. Edits the SELECTED slot of this\n"
                                   "lot IN PLACE (affects every asset pointing at this lot)."));

    static char status[160] = "";
    ImGui::BeginDisabled(lot == 0);
    if (ImGui::Button(tr("Set item on this asset's lot")))
    {
        bool ok = goblin::overlay_api::param_set_field("ItemLotParam_map", (uint64_t)lot,
                                                       field, (double)new_item);
        if (ok)
            goblin::overlay_api::we_bundle_record_set("ItemLotParam_map", (uint64_t)lot, field,
                                                      (double)new_item);
        std::snprintf(status, sizeof(status),
                      ok ? "ok: lot %d %s = %d (Refresh markers to see it)"
                         : "FAILED to set lot %d %s", lot, field, new_item);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("IN-PLACE re-skin of the selected slot: changes the shared lot\n"
                                   "itself, so EVERY asset that points at this lot changes too. To\n"
                                   "change only THIS asset, use Repoint below instead. Slot 1 is\n"
                                   "what the map marker shows."));

    // ── Slice 2: repoint this asset at a DIFFERENT existing lot ──────────────────────────────────
    // Non-destructive: leaves the current (shared) lot untouched and just points this one asset's
    // pickUpItemLotParamId elsewhere. Live preview shows what the target lot yields before committing.
    ImGui::Spacing();
    static int target_lot = 0;
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("Repoint to lot"), &target_lot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Point THIS asset at a different ItemLotParam_map row\n"
                                   "(non-destructive — the current lot is left unchanged). Works for\n"
                                   "an existing lot or one you just cloned below; Refresh markers\n"
                                   "re-reads the lot table so a cloned lot resolves."));

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
        if (ok)
            goblin::overlay_api::we_bundle_record_set("AssetEnvironmentGeometryParam", (uint64_t)aeg,
                                                      "pickUpItemLotParamId", (double)target_lot);
        std::snprintf(status, sizeof(status),
                      ok ? "ok: asset %d pickUpItemLotParamId = %d (Refresh markers to see it)"
                         : "FAILED to repoint asset %d", aeg, target_lot);
    }
    ImGui::EndDisabled();

    // ── Slice 5: CLONE this lot to a new id ──────────────────────────────────────────────────────
    // Copies the current lot into a fresh ItemLotParam_map row so you can edit it WITHOUT touching
    // the shared original. The new lot has no asset pointing at it, so clone → repoint an asset at it
    // (fills "Repoint to lot") → Refresh markers, which resets the LotReader so the new lot resolves.
    ImGui::Spacing();
    static int clone_new_lot = 900900900;  // a high, normally-unused ItemLotParam_map id
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt(tr("Clone to new lot id"), &clone_new_lot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("New ItemLotParam_map row id for the clone (must be unused).\n"
                                   "After cloning, this id is filled into 'Repoint to lot' so you\n"
                                   "can point this asset at the copy, then Refresh markers."));
    ImGui::BeginDisabled(lot == 0 || clone_new_lot <= 0);
    if (ImGui::Button(tr("Clone this lot")))
    {
        bool ok = goblin::overlay_api::param_clone("ItemLotParam_map", (uint64_t)lot,
                                                   (int32_t)clone_new_lot);
        if (ok)
        {
            goblin::overlay_api::we_bundle_record_clone("ItemLotParam_map", (uint64_t)lot,
                                                        (int32_t)clone_new_lot);
            target_lot = clone_new_lot;  // pre-fill the repoint target with the fresh copy
        }
        std::snprintf(status, sizeof(status),
                      ok ? "ok: cloned lot %d -> %d (Repoint to it, then Refresh markers)"
                         : "FAILED to clone lot %d (id in use / not found?)", lot, clone_new_lot);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Add a new lot row that copies this one. Non-destructive: the\n"
                                   "original lot is untouched. Then Repoint + Refresh markers."));

    // ── Move a placement (live geom transform setter; MSB-write-free) ────────────────────────────
    // Nudge the geom instance NEAREST the player by a delta via the engine's own setter (vtable[0xd0]).
    // Live-only (not persisted). Picks the nearest loaded CSWorldGeomIns to the player — approximate
    // across tiles (the transform is block-local), so it's a "grab the closest prop and move it" tool.
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Move a placement (live)"));
    static float mv[3] = {0.0f, 5.0f, 0.0f};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputFloat3(tr("Delta X/Y/Z"), mv);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("World-units to move the nearest placement by (Y is up). Uses the\n"
                                   "engine's transform setter — the object really moves + collides.\n"
                                   "Not saved (a live edit); use Restore to put it back."));
    // Primary: move the SELECTED asset's nearest placement (the aeg picked above).
    ImGui::BeginDisabled(aeg <= 0);
    if (ImGui::Button(tr("Move this asset")))
    {
        float before[3] = {}, now[3] = {};
        bool ok = goblin::overlay_api::we_move_aeg(aeg, mv[0], mv[1], mv[2], before, now);
        if (ok)
            std::snprintf(status, sizeof(status),
                          "moved asset %d: (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)", aeg,
                          before[0], before[1], before[2], now[0], now[1], now[2]);
        else
            std::snprintf(status, sizeof(status), "no loaded placement for asset %d nearby", aeg);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Move the nearest loaded placement of the picked asset (aegRow above).\n"
                                   "Targets THAT object, not just whatever is closest to you."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Move nearest")))
    {
        float before[3] = {}, now[3] = {}, dist = -1.0f;
        bool ok = goblin::overlay_api::we_move_near(mv[0], mv[1], mv[2], before, now, &dist);
        if (ok)
            std::snprintf(status, sizeof(status),
                          "moved nearest (dist %.0f): (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)", dist,
                          before[0], before[1], before[2], now[0], now[1], now[2]);
        else
            std::snprintf(status, sizeof(status), "move failed (in-world? geom loaded?)");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Restore moved")))
    {
        bool ok = goblin::overlay_api::we_move_restore();
        std::snprintf(status, sizeof(status), ok ? "restored the moved placement" : "nothing to restore");
    }

    // ── Slice 7: save the edits as a world bundle (persists across restarts; vision #1) ──────────
    // Every edit above is recorded into an in-memory bundle; Save writes it to <mod>/world_bundle.toml,
    // which re-applies automatically on the next launch. Apply re-runs the saved bundle now.
    ImGui::Spacing();
    ImGui::SeparatorText(tr("World bundle (persist edits)"));
    ImGui::TextDisabled("%zu %s", goblin::overlay_api::we_bundle_count(),
                        tr("edit(s) recorded — save to keep them across restarts"));
    if (ImGui::Button(tr("Save bundle")))
    {
        bool ok = goblin::overlay_api::we_bundle_save();
        std::snprintf(status, sizeof(status),
                      ok ? "ok: saved world_bundle.toml (%zu ops) — re-applies on next launch"
                         : "FAILED to save world_bundle.toml",
                      goblin::overlay_api::we_bundle_count());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Apply bundle")))
    {
        int n = goblin::overlay_api::we_bundle_apply();
        goblin::overlay_api::rebuild_markers();
        std::snprintf(status, sizeof(status), "applied %d bundle op(s) — Refresh markers running", n);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear bundle")))
    {
        goblin::overlay_api::we_bundle_clear();
        std::snprintf(status, sizeof(status), "cleared the in-memory bundle (the saved file is kept)");
    }

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

    // ── Slice 6: browsable asset / item PICKER ───────────────────────────────────────────────────
    // Instead of typing raw ids, scan the live params once into cached lists and pick from a filtered
    // list — clicking sets the Asset (aegRow) or New goods id fields above. Scan is on-demand (a brief
    // hitch); the filter runs client-side over the cached copy each frame (cheap).
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(tr("Browse (pick asset / item)")))
    {
        using goblin::world_editor::WEAsset;
        using goblin::world_editor::WEGoods;
        static std::vector<WEAsset> assets;
        static std::vector<WEGoods> goods;
        static char af[64] = "";
        static char gf[64] = "";

        auto matches = [](const char *name, int a, int b, const char *filt) {
            if (!filt[0]) return true;
            char hay[160];
            std::snprintf(hay, sizeof(hay), "%s %d %d", name, a, b);
            std::string h(hay), f(filt);
            for (char &c : h) c = (char)std::tolower((unsigned char)c);
            for (char &c : f) c = (char)std::tolower((unsigned char)c);
            return h.find(f) != std::string::npos;
        };

        if (ImGui::Button(tr("Scan world")))
        {
            goblin::overlay_api::we_scan();
            assets.resize(goblin::overlay_api::we_asset_count());
            if (!assets.empty())
                assets.resize(goblin::overlay_api::we_copy_assets(assets.data(), assets.size()));
            goods.resize(goblin::overlay_api::we_goods_count());
            if (!goods.empty())
                goods.resize(goblin::overlay_api::we_copy_goods(goods.data(), goods.size()));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d assets / %d items", (int)assets.size(), (int)goods.size());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Reads every pickup asset + named goods from the live params.\n"
                                       "One-shot (brief hitch); re-scan after cloning to pick the copy."));

        // Pickup assets → sets the Asset (aegRow) field above.
        ImGui::SeparatorText(tr("Pickup assets"));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##af", tr("filter: item name / aeg / lot"), af, sizeof(af));
        if (ImGui::BeginChild("we_assets", ImVec2(0, 120), true))
        {
            int shown = 0;
            for (const auto &a : assets)
            {
                if (!matches(a.name, a.aegRow, a.lot, af)) continue;
                if (shown++ >= 300) { ImGui::TextDisabled("%s", tr("… refine the filter")); break; }
                char label[144];
                std::snprintf(label, sizeof(label), "aeg %d   lot %d   %s", a.aegRow, a.lot,
                              a.name[0] ? a.name : "(no name)");
                if (ImGui::Selectable(label)) aeg = a.aegRow;
            }
        }
        ImGui::EndChild();

        // Goods → sets the New goods id field above.
        ImGui::SeparatorText(tr("Items (goods)"));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##gf", tr("filter: name / id"), gf, sizeof(gf));
        if (ImGui::BeginChild("we_goods", ImVec2(0, 120), true))
        {
            int shown = 0;
            for (const auto &gd : goods)
            {
                if (!matches(gd.name, gd.goodsId, gd.goodsId, gf)) continue;
                if (shown++ >= 300) { ImGui::TextDisabled("%s", tr("… refine the filter")); break; }
                char label[144];
                std::snprintf(label, sizeof(label), "%d   %s", gd.goodsId, gd.name);
                if (ImGui::Selectable(label)) new_item = gd.goodsId;
            }
        }
        ImGui::EndChild();
    }
}
}  // namespace goblin::overlay::panel
