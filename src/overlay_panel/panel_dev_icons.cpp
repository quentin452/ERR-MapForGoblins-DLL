// F1 panel — dev/RE icon sections: "Item icons (P2b test)", "Icon migration (Baked → GPU)",
// "ERR map sprites (GPU debug)", "Grace texture debug (live)". Moved verbatim from
// goblin_overlay_render.cpp::draw_panel in the split (big-files refactor item 1). The first,
// third and fourth are gated behind dump_icon_textures (dev-only); the migration census is
// always available.

#include "panel_internal.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_map_data.hpp"                       // generated::Category
#include "worldmap/category_meta.hpp"                // category_gpu_icon_name / rep icons
#include "worldmap/map_renderer.hpp"                 // category_color
#include "generated_shared/goblin_overlay_icons.hpp" // ATLAS cells (baked_uv)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstring>
#include <vector>

namespace goblin::overlay::panel
{
void draw_dev_icon_sections(const OverlayFrameCtx &ctx, Filter &f)
{
    // P2b vertical slice: draw live-harvested item icons (copied GPU→GPU from the engine's
    // menu sheets). Open the inventory first to harvest, then check here. Dev-gated.
    const bool show_p2b = (*goblin::overlay_api::cfg_dumpIconTextures_ptr()) &&
                          f.match("item icons p2b test harvest inventory force-load csfile "
                                  "bind flip group createimage grace dev");
    if (f.filtering && show_p2b) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_p2b && ImGui::CollapsingHeader("Item icons (P2b test)"))
    {
        // Draw the ACTUAL harvested icons (a hardcoded id list may not match what the
        // player browsed → empty grid even with harvested>0). Exercises the batch path.
        ImGui::Text("harvested: %zu  (open inventory to fill)", goblin::overlay_api::harvested_count());
        std::vector<int> ids = goblin::overlay_api::harvested_ids(48);
        int drawn = 0;
        for (int id : ids)
        {
            UINT64 h = ensure_item_icon_srv(id);
            if (!h) continue;
            if (drawn++ % 8 != 0) ImGui::SameLine();
            // The copied tex is the 4-block-snapped region; crop to the exact icon via UVs.
            auto sit = ctx.item_icon_srvs->find(id);
            ImVec2 uv0 = sit != ctx.item_icon_srvs->end() ? sit->second.uv0 : ImVec2(0, 0);
            ImVec2 uv1 = sit != ctx.item_icon_srvs->end() ? sit->second.uv1 : ImVec2(1, 1);
            ImGui::Image(reinterpret_cast<ImTextureID>(h), ImVec2(48, 48), uv0, uv1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("iconId %d", id);
        }
        if (!drawn)
            ImGui::TextDisabled(ids.empty() ? "no icons harvested yet — open inventory first"
                                            : "harvested icons not ready yet (1-frame batch)...");

        // DEV force-load test (findings §5b): stream a TPF resident by FD4 path via CSFile.
        // Type a path + click → watch [FORCELOAD] in the log (handle != 0 = loaded) and whether
        // the sheet becomes resident. e.g. "menu:/00_Solo.tpf", "menutpfbnd:/00_Solo/<name>.tpf".
        ImGui::Separator();
        static char s_fl_path[192] = "menu:/00_Solo.tpf";
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("##flpath", s_fl_path, sizeof(s_fl_path));
        ImGui::SameLine();
        if (ImGui::Button("Force-load (CSFile)"))
            goblin::overlay_api::force_load_file(s_fl_path);
        // One-click sweep of the menu resource-GROUP BNDs (re_v110): logs a [FORCELOAD] line per
        // path. Try with the map/inventory CLOSED so a non-resident group's load is visible
        // (handle != 0 + harvested grows after you then open the inventory).
        if (ImGui::Button("Test: force-load common groups"))
        {
            static const char *kGroups[] = {
                "menu:/01_Common.tpfbhd", "menu:/01_Common.tpf",
                "menu:/00_Solo.tpfbhd",   "menu:/03_ChrMake.tpfbhd",
                "menu:/71_MapTile.tpfbhd", "menu:/02_Title.tpfbhd",
            };
            for (const char *p : kGroups) goblin::overlay_api::force_load_file(p);
        }
        // Map-point icon rects (MENU_MAP_* → iconId→rect) are harvested from the resident image
        // repo by the repo walk when the world map opens — no force-load needed.
        ImGui::Text("map-icon layout entries: %zu", goblin::overlay_api::map_icon_layout_count());

        // ── Bind-flip test (findings §5e) — does +0x7c flip trigger the per-icon bind? ──
        // Recommended sequence (watch [BINDTEST] in the log, then "harvested:" above):
        //   0) open inventory/map ONCE so the ticker captures the manager.
        //   1) "Dump groups" — logs each loaded group's apply-vmethod RVA + flags.
        //   2) "Load files (gid)" — streams the group's TPF+sblytbnd resident via the by-id loaders.
        //   3) "Flip-bind all" — sets +0x7c on every loaded group → engine re-applies this tick.
        //   4) re-check "harvested:" — if it JUMPS for un-browsed items, the flag IS the bind lever.
        ImGui::Separator();
        ImGui::TextDisabled("Bind-flip test (§5e): open inventory once, then ->");
        static int s_bt_gid = 1;   // 1 = 01_Common (item-icon group)
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("group id", &s_bt_gid);
        if (s_bt_gid < 0) s_bt_gid = 0; if (s_bt_gid > 8) s_bt_gid = 8;
        if (ImGui::Button("1) Dump groups"))       goblin::overlay_api::bind_test(1, s_bt_gid);
        ImGui::SameLine();
        if (ImGui::Button("2) Load files (gid)"))  goblin::overlay_api::bind_test(2, s_bt_gid);
        if (ImGui::Button("3) Flip-bind all"))      goblin::overlay_api::bind_test(3, s_bt_gid);
        ImGui::SameLine();
        if (ImGui::Button("4) Load + flip (gid)")) goblin::overlay_api::bind_test(4, s_bt_gid);

        // Force-CreateImage (§5g): replay the GFx per-image bind callback for one item icon.
        // Watch [CREATEIMG] in the log: live names while browsing, then the forced result +
        // whether "harvested:" grows. Open inventory once first so the context is captured.
        ImGui::Separator();
        ImGui::TextDisabled("Force-bind one icon via CreateImage (§5g):");
        static int s_ci_icon = 0;
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("iconId##ci", &s_ci_icon);
        ImGui::SameLine();
        if (ImGui::Button("Force CreateImage")) goblin::overlay_api::force_create_icon(s_ci_icon);
        if (ImGui::Button("Replay last live symbol (control)")) goblin::overlay_api::force_create_last();
        if (ImGui::Button("Force graces now")) goblin::overlay_api::force_graces();
        ImGui::SameLine();
        ImGui::TextDisabled("(re-harvest MENU_MAP_*/SB_ERR_Grace_* on demand)");

        // LOAD DIRECT: take ER's OWN already-loaded sheet resource (the harvested grace
        // sheet's ID3D12Resource) and copy the WHOLE sheet straight into our own texture.
        // No file, no DCX — the game's resident GPU image, grabbed directly.
        ImGui::Separator();
        ImGui::TextDisabled("Copy ER's loaded sheet DIRECT into our texture:");
        static UINT64 s_dir_tex = 0; static int s_dir_w = 0, s_dir_h = 0;
        if (ImGui::Button("Load ER image direct"))
            s_dir_tex = copy_er_sheet_direct(s_dir_w, s_dir_h);
        if (s_dir_tex)
        {
            ImGui::Text("%dx%d (our copy of ER's sheet)", s_dir_w, s_dir_h);
            float dw = s_dir_w > 320 ? 320.f : (float)s_dir_w;
            float dh = s_dir_h ? dw * s_dir_h / s_dir_w : dw;
            ImGui::Image((ImTextureID)s_dir_tex, ImVec2(dw, dh));
        }

        // LOAD FROM RAM: upload the DDS the Oodle hook captured in the game's decompressed
        // RAM (FD4 cache) into our own texture — no game GPU bind, mod-agnostic. Proves the
        // whole chain: DCX→oodle→TPF(RAM)→our SRV→draw.
        // BROWSE every DDS we grabbed from ER's decompresses (RAM, no game bind). Flip through
        // them to find the icon sheet. Each "Show" uploads that DDS into our own texture.
        ImGui::Separator();
        size_t ddsN = goblin::overlay_api::tpf_dds_count();
        ImGui::TextDisabled("Captured ER DDS in RAM: %zu (browse to find the icon sheet)", ddsN);
        static int s_idx = 0;
        static UINT64 s_ram_tex = 0; static int s_ram_w = 0, s_ram_h = 0, s_ram_fmt = 0;
        if (ddsN)
        {
            if (s_idx >= (int)ddsN) s_idx = (int)ddsN - 1;
            ImGui::SetNextItemWidth(180);
            ImGui::SliderInt("##ddsidx", &s_idx, 0, (int)ddsN - 1);
            ImGui::SameLine(); if (ImGui::SmallButton("<") && s_idx > 0) --s_idx;
            ImGui::SameLine(); if (ImGui::SmallButton(">") && s_idx < (int)ddsN - 1) ++s_idx;
            ImGui::SameLine();
            if (ImGui::Button("Show"))
            {
                static std::vector<uint8_t> dds;
                if (goblin::overlay_api::tpf_dds_at((size_t)s_idx, dds))
                {
                    DXGI_FORMAT fdds;
                    s_ram_tex = create_tex_from_dds_mem(dds.data(), dds.size(), s_ram_w, s_ram_h, fdds);
                    s_ram_fmt = (int)fdds;
                }
            }
        }
        if (s_ram_tex)
        {
            ImGui::Text("#%d  %dx%d fmt=%d (ER RAM, no bind)", s_idx, s_ram_w, s_ram_h, s_ram_fmt);
            float dw = s_ram_w > 320 ? 320.f : (float)s_ram_w;
            float dh = s_ram_h ? dw * s_ram_h / s_ram_w : dw;
            ImGui::Image((ImTextureID)s_ram_tex, ImVec2(dw, dh));
        }
    }

    // Icon migration (Baked → GPU): a LIVE three-way census of what each category ACTUALLY
    // draws RIGHT NOW — GPU icon (a real native engine sprite), Atlas (the baked PNG cell), or
    // Circle (no cell → coloured circle). This mirrors map_renderer's IconSet::resolve order,
    // including live residency (the GPU path only "wins" once its sprite is harvested), so the
    // panel shows the truth — not just what's WIRED. Lets us verify the per-category item icon
    // (00_Solo atlas) is correct before committing to the group-load that makes it resident.
    // Dev-gated (like the P2b / sprites / grace-debug siblings above): the census is an
    // RE/verification tool, not a user setting → only surfaces when "dump icon textures" is on.
    const bool show_mig = (*goblin::overlay_api::cfg_dumpIconTextures_ptr()) &&
                          f.match("icon migration baked gpu native atlas circle census "
                                  "category render source");
    if (f.filtering && show_mig) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_mig && ImGui::CollapsingHeader("Icon migration (Baked \xE2\x86\x92 GPU)"))
    {
        namespace wm = goblin::worldmap;
        const bool native_on = (*goblin::overlay_api::cfg_nativeItemIcons_ptr());
        const float SZ = 22.f;
        const ImVec4 GREEN(0.40f, 0.85f, 0.45f, 1), YEL(0.90f, 0.80f, 0.35f, 1),
                     GRAY(0.62f, 0.62f, 0.62f, 1);
        // Runtime native sprite (tex+UV) for a category, using the SAME overlay backends the
        // renderer draws with → the panel shows the EXACT runtime icon. false = none right now.
        auto runtime_tex = [&](int c, ImTextureID &tex, ImVec2 &uv0, ImVec2 &uv1,
                               const char *&via) -> bool {
            via = ""; if (!native_on) return false;
            void *t = nullptr; float u0, v0, u1, v1; bool ok = false;
            if (const char *sym = wm::category_gpu_icon_name(c))
                ok = goblin::overlay_api::native_map_point_icon_by_name(sym, t, u0, v0, u1, v1), via = "name symbol";
            if (!ok) if (int iid = wm::category_gpu_iconId(c))
                ok = goblin::overlay_api::native_map_point_icon(iid, t, u0, v0, u1, v1), via = "map-point id";
            if (!ok) if (int rep = wm::category_rep_icon(c))
                ok = goblin::overlay_api::native_item_icon(rep, t, u0, v0, u1, v1), via = "item icon";
            if (!ok) { via = ""; return false; }
            tex = reinterpret_cast<ImTextureID>(t); uv0 = ImVec2(u0, v0); uv1 = ImVec2(u1, v1);
            return true;
        };
        // Baked atlas UV for a category's cell (mirror map_renderer's icon_uv). false = no cell.
        auto baked_uv = [&](int c, ImVec2 &uv0, ImVec2 &uv1) -> bool {
            using namespace goblin::overlay_icons;
            const char *key = wm::category_icon_key(c);
            if (!key) return false;
            for (int i = 0; i < ICON_CELL_COUNT; ++i)
                if (std::strcmp(ICON_CELLS[i].key, key) == 0)
                {
                    const IconCell &cell = ICON_CELLS[i];
                    uv0 = ImVec2((cell.col * CELL) / (float)ATLAS_W, (cell.row * CELL) / (float)ATLAS_H);
                    uv1 = ImVec2(((cell.col + 1) * CELL) / (float)ATLAS_W, ((cell.row + 1) * CELL) / (float)ATLAS_H);
                    return true;
                }
            return false;
        };
        const bool grace_gpu = native_on && (*goblin::overlay_api::cfg_graceOverlay_ptr());
        const int total = wm::category_count();
        struct Row { int s; const char *via; bool hasRt; ImTextureID rt; ImVec2 ra, rb; };
        std::vector<Row> rows(total);
        int gpu = 0, atlas = 0, circle = 0;
        for (int c = 0; c < total; ++c)
        {
            Row &r = rows[c];
            r.hasRt = runtime_tex(c, r.rt, r.ra, r.rb, r.via);
            if (r.hasRt) r.s = 2;
            else if (grace_gpu && c == static_cast<int>(goblin::generated::Category::WorldGraces))
            { r.s = 2; r.via = "grace sprite"; }
            else r.s = wm::category_has_baked_icon(c) ? 1 : 0;
            switch (r.s) { case 2: ++gpu; break; case 1: ++atlas; break; default: ++circle; }
        }
        ImGui::Text("Live render source (of %d categories)%s:", total,
                    native_on ? "" : "  [native_item_icons OFF]");
        ImGui::TextColored(GREEN, "  GPU icon : %d", gpu);
        ImGui::SameLine(); ImGui::TextColored(YEL, "   Atlas : %d", atlas);
        ImGui::SameLine(); ImGui::TextColored(GRAY, "   Circle : %d", circle);
        ImGui::ProgressBar(total ? (float)gpu / total : 0.f, ImVec2(-1, 0));
        ImGui::TextDisabled("A/B: [baked]  [runtime]   category  (source)");
        ImGui::Separator();
        if (ImGui::BeginChild("iconmig_census", ImVec2(0, 320), true))
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            for (int c = 0; c < total; ++c)
            {
                const Row &r = rows[c];
                const char *name = goblin::overlay_api::category_label(c);
                // [baked] thumbnail (atlas cell, or a coloured circle for the no-cell categories).
                ImVec2 bu0, bu1, p = ImGui::GetCursorScreenPos();
                if (ctx.atlas_srv && baked_uv(c, bu0, bu1))
                    ImGui::Image(reinterpret_cast<ImTextureID>(ctx.atlas_srv), ImVec2(SZ, SZ), bu0, bu1);
                else
                {
                    ImGui::Dummy(ImVec2(SZ, SZ));
                    dl->AddCircleFilled(ImVec2(p.x + SZ / 2, p.y + SZ / 2), SZ * 0.34f,
                                        goblin::worldmap::category_color(c));
                }
                ImGui::SameLine();
                // [runtime] thumbnail (the real native sprite) or an empty slot if not resident.
                ImVec2 q = ImGui::GetCursorScreenPos();
                if (r.hasRt)
                    ImGui::Image(r.rt, ImVec2(SZ, SZ), r.ra, r.rb);
                else
                {
                    ImGui::Dummy(ImVec2(SZ, SZ));
                    dl->AddRect(q, ImVec2(q.x + SZ, q.y + SZ), IM_COL32(110, 110, 110, 90));
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                const ImVec4 col = r.s == 2 ? GREEN : r.s == 1 ? YEL : GRAY;
                const bool wired = wm::category_is_gpu_native(c);
                if (r.s == 2)
                    ImGui::TextColored(col, "%s  (%s)", name ? name : "?", r.via);
                else
                    ImGui::TextColored(col, "%s%s", name ? name : "?",
                                       (native_on && wired) ? "  (wired, not resident)" : "");
            }
        }
        ImGui::EndChild();
    }

    // Grace-sprite GPU debug: draw every harvested SB_ERR_Grace_* frame (full-sheet SRV +
    // UV, no copy) so we can visually pick the correct grace rect. Open the world map (with a
    // discovered grace) to populate. The frame that looks like a Site of Grace = the one to lock.
    const bool show_sprites = (*goblin::overlay_api::cfg_dumpIconTextures_ptr()) &&
                              f.match("err map sprites gpu debug grace sheet frame harvest dev");
    if (f.filtering && show_sprites) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_sprites && ImGui::CollapsingHeader("ERR map sprites (GPU debug)"))
    {
        ensure_grace_debug();
        grace_candidate_gate_warning();
        const std::vector<GraceDbg> &grace_dbg_view = grace_debug_candidates();
        if (grace_dbg_view.empty())
            ImGui::TextDisabled("open the world map to harvest SB_ERR_* map sprites");
        for (const GraceDbg &d : grace_dbg_view)
        {
            ImGui::Image(d.tex, ImVec2(64, 64), d.uv0, d.uv1);
            ImGui::SameLine();
            ImGui::Text("%s  %dx%d  uv(%.3f,%.3f)-(%.3f,%.3f)", d.name.c_str(), d.w, d.h,
                        d.uv0.x, d.uv0.y, d.uv1.x, d.uv1.y);
        }
    }

    // Grace texture DEBUG (live format/swizzle/source) — verify the active grace mapped the
    // right NAME and the right COLORING. Tweak below + "Re-apply" re-copies the SRV live.
    const bool show_gracedbg = (*goblin::overlay_api::cfg_dumpIconTextures_ptr()) &&
                               f.match("grace texture debug live srgb swizzle channels "
                                       "format candidate source dev");
    if (f.filtering && show_gracedbg) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_gracedbg && ImGui::CollapsingHeader("Grace texture debug (live)"))
    {
        // The active grace, drawn big (this IS what the map markers use).
        if (grace_state() == 1)
        {
            void *dbg_gpu; ImVec2 dbg_uv0, dbg_uv1; int dbg_nw, dbg_nh;
            grace_srv_info(dbg_gpu, dbg_uv0, dbg_uv1, dbg_nw, dbg_nh);
            ImGui::Text("active grace  fmt=%d (98=BC7_UNORM 99=BC7_SRGB)", grace_dbg_fmt_used());
            ImGui::Image(reinterpret_cast<ImTextureID>(dbg_gpu), ImVec2(128, 128),
                         dbg_uv0, dbg_uv1);
            ImGui::SameLine();
            // Full copied texture (no UV crop) so a wrong-rect vs wrong-color bug is separable.
            ImGui::Image(reinterpret_cast<ImTextureID>(dbg_gpu), ImVec2(128, 128));
            ImGui::TextDisabled("left = UV-cropped (marker) | right = full copied block");
        }
        else ImGui::TextDisabled("grace not ready (open map near a grace)");

        bool dirty = false;
        ImGui::Text("sRGB:");
        ImGui::SameLine(); dirty |= ImGui::RadioButton("auto##gs", grace_dbg_srgb_ptr(), 0);
        ImGui::SameLine(); dirty |= ImGui::RadioButton("UNORM(98)##gs", grace_dbg_srgb_ptr(), 1);
        ImGui::SameLine(); dirty |= ImGui::RadioButton("SRGB(99)##gs", grace_dbg_srgb_ptr(), 2);
        ImGui::Text("channels:");
        ImGui::SameLine(); dirty |= ImGui::RadioButton("RGBA##gz", grace_dbg_swiz_ptr(), 0);
        ImGui::SameLine(); dirty |= ImGui::RadioButton("R<->B##gz", grace_dbg_swiz_ptr(), 1);
        ImGui::SameLine(); dirty |= ImGui::RadioButton("R<->G##gz", grace_dbg_swiz_ptr(), 2);
        ImGui::SameLine(); dirty |= ImGui::RadioButton("A=1##gz", grace_dbg_swiz_ptr(), 3);
        if (ImGui::Button("Re-apply (rebuild grace SRV)") || dirty)
            force_rebuild_grace();

        // Source picker: which captured candidate is the active grace (test "the right name").
        ImGui::Separator();
        ImGui::TextDisabled("source candidate (click to use as grace):");
        grace_candidate_gate_warning();
        std::vector<goblin::GraceCandidate> cands = goblin::overlay_api::grace_candidates();
        for (size_t i = 0; i < cands.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("use")) { goblin::overlay_api::set_grace_from_candidate(i); force_rebuild_grace(); }
            ImGui::SameLine();
            ImGui::Text("%s  (%d,%d)-(%d,%d)", cands[i].name.c_str(), cands[i].spr.x0,
                        cands[i].spr.y0, cands[i].spr.x1, cands[i].spr.y1);
            ImGui::PopID();
        }
    }
}
} // namespace goblin::overlay::panel
