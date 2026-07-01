// Phase 2 (Slice B) of docs/plans/overlay_hot_reload_playwright_plan.md: the ImGui draw layer,
// extracted from src/goblin_overlay.cpp into its own translation unit. See
// src/goblin_overlay_render.hpp for the OverlayFrameCtx boundary + wrapper-function surface this
// file uses to reach host-owned D3D12 state (device/queue/heap/frames) that stays in
// goblin_overlay.cpp — those helpers (ensure_grace_srv, ensure_item_icon_srv, etc.) turned out to
// be genuinely D3D12-coupled, not self-contained, during the Slice B move (see the plan doc).
//
// Phase 2 Slice C: goblin_overlay_render_api.hpp is the consolidated wrapper surface for
// everything else this file needs from host-only code (config/ui/worldmap_probe/etc.).

#include "goblin_overlay_render.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_overlay.hpp"
#include "goblin_config.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_debug_events.hpp"

#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <spdlog/spdlog.h>

#include "goblin_inject.hpp"             // goblin::overlay_api::world_map_open()
#include "goblin_markers.hpp"            // goblin::overlay_api::markers_set_event_flag()
#include "goblin_worldmap_probe.hpp"     // get_live_view() for the marker prototype
#include "goblin_map_data.hpp"           // generated::MAP_ENTRIES / Category
#include "worldmap/grace_layer.hpp"      // goblin::worldmap::GraceLayer
#include "worldmap/quest_npc_layer.hpp"  // goblin::worldmap::QuestNpcLayer
#include "worldmap/map_entry_layer.hpp"  // goblin::worldmap::MapEntryLayer
#include "worldmap/map_renderer.hpp"     // goblin::worldmap::render_markers
#include "worldmap/category_meta.hpp"    // baked→GPU icon migration counters (F1 panel)
#include "worldmap/loot_disk.hpp"        // disk_loot_state — F1 "maps not found" error
#include "worldmap/name_fmg_en.hpp"      // lookup_name_en_disk_utf8 — F1 English search aliases
#include "re_signatures.hpp"             // sig_health — F1 "signatures unresolved" error
#include "goblin_messages.hpp"           // lookup_text_utf8 (item-search name resolution)
#include "generated_shared/goblin_overlay_icons.hpp" // ATLAS_PNG category-icon atlas
#include "goblin_bench.hpp"              // GOBLIN_BENCH scoped timers
#include "input/input_shared.hpp"        // goblin::overlay_api::input_menu_open()
#include "input/input_cursor.hpp"        // goblin::overlay_api::get_cursor_pos_real()

#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace
{
    void append_folded(std::string &out, uint32_t cp)
    {
        if (cp < 0x80) { out += (char)tolower((int)cp); return; }
        switch (cp)
        {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: out += 'a'; break;
        case 0xC6: case 0xE6: out += "ae"; break;
        case 0xC7: case 0xE7: out += 'c'; break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: out += 'e'; break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: out += 'i'; break;
        case 0xD0: case 0xF0: out += 'd'; break;
        case 0xD1: case 0xF1: out += 'n'; break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: out += 'o'; break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: out += 'u'; break;
        case 0xDD: case 0xFD: case 0xFF: out += 'y'; break;
        case 0xDE: case 0xFE: out += "th"; break;
        case 0xDF: out += "ss"; break;
        case 0x152: case 0x153: out += "oe"; break;  // Œ / œ
        default: break;  // no ASCII base — drop
        }
    }

    // Decode UTF-8 `s` into a lowercase, accent-folded ASCII string (see append_folded).
    std::string fold_ci(const char *s)
    {
        std::string out;
        const unsigned char *p = (const unsigned char *)s;
        while (p && *p)
        {
            unsigned char c = *p;
            uint32_t cp;
            int len;
            if (c < 0x80)            { cp = c;        len = 1; }
            else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
            else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
            else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
            else { ++p; continue; }  // stray continuation / invalid lead — skip
            int i = 1;
            for (; i < len; ++i)
            {
                if ((p[i] & 0xC0) != 0x80) break;  // truncated sequence
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            p += i;  // advance by bytes actually consumed
            append_folded(out, cp);
        }
        return out;
    }

    // Case-insensitive, accent-insensitive substring match (empty needle = match all).
    // Used by the Sections & categories search box (Quest Browser has its own copy).
    bool contains_ci(const char *hay, const char *need)
    {
        if (!need || !need[0]) return true;
        return fold_ci(hay).find(fold_ci(need)) != std::string::npos;
    }

    // Word-order-independent match for the item search: every whitespace-separated
    // token of `query` must appear (case/accent-insensitive substring) somewhere in
    // `hay`. So "Claw Talisman", "Talisman Claw" and "griffe claw" all match the same
    // marker when `hay` is its combined game-language + English text. Empty query (no
    // tokens) returns false — callers guard the empty box separately.
    bool matches_all_tokens(const std::string &hay, const char *query)
    {
        const std::string fh = fold_ci(hay.c_str());
        bool any = false;
        for (const char *p = query; *p;)
        {
            while (*p == ' ' || *p == '\t') ++p;
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (p == start) break;
            any = true;
            if (fh.find(fold_ci(std::string(start, p - start).c_str())) == std::string::npos)
                return false;
        }
        return any;
    }

    bool g_large = true;         // false = compact widget, true = full panel

    void grace_candidate_gate_warning()
    {
        if ((*goblin::overlay_api::cfg_graceOverlay_ptr()) && (*goblin::overlay_api::cfg_graceGpuSprite_ptr()))
            return;
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.15f, 1.0f),
            "(!) Few/no candidates listed?\n"
            "    The forced MENU_MAP_* grace sprites are only created while BOTH\n"
            "    'grace_overlay' AND 'grace_gpu_sprite' are ON. Enable them via the\n"
            "    checkboxes below (Overlay graces / live engine sprite), or set both\n"
            "    = true in MapForGoblins.ini. Otherwise only the live SB_ERR_Grace_*\n"
            "    frame the game happens to draw will appear.");
    }

    bool scale_control(const char *label, float *v, float lo, float hi,
                       float step, float step_fast, const char *fmt)
    {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(150.0f);
        changed |= ImGui::SliderFloat("##slider", v, lo, hi, fmt);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        changed |= ImGui::InputFloat("##input", v, step, step_fast, fmt);
        if (*v < lo) *v = lo;
        if (*v > hi) *v = hi;
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();
        return changed;
    }

    // On-screen keyboard for gamepad text entry (dx-bugs-backlog PR C-2 part 2). ImGui's gamepad
    // nav (enabled in PR C-2 part 1) has no answer for InputText, so this draws a popup made of
    // ordinary buttons — D-pad/stick already moves focus between them and A already activates
    // them, for free, via the SAME nav we already enabled. No custom input polling here at all.
    // Writes directly into the caller's InputText buffer; next frame InputTextWithHint just
    // re-renders whatever's in it, same as if the user had typed it on a real keyboard.
    void draw_gamepad_keyboard_button(const char *popup_id, char *buf, size_t buf_size)
    {
        static const char *const ALPHA_ROWS[] = {"ABCDEFGHIJK", "LMNOPQRSTUV", "WXYZ"};
        static const char *const QWERTY_ROWS[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        const char *const *rows = ((*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) == 1) ? QWERTY_ROWS : ALPHA_ROWS;

        ImGui::PushID(popup_id);
        // NOT SameLine(): the InputText above is sized to GetContentRegionAvail().x (100% width),
        // so a same-line button would land past the panel's right edge — invisible without
        // scrolling. Own line instead, always visible regardless of the field's width.
        if (ImGui::SmallButton("Kbd")) ImGui::OpenPopup(popup_id);   // ASCII-only: U+2328 isn't in the merged font ranges
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On-screen keyboard for gamepad text entry.");

        if (ImGui::BeginPopup(popup_id))
        {
            const size_t len = strlen(buf);
            for (int r = 0; r < 3; ++r)
            {
                for (const char *c = rows[r]; *c; ++c)
                {
                    ImGui::PushID(static_cast<int>(c - rows[r]) + r * 100);
                    char label[2] = {*c, '\0'};
                    if (ImGui::Button(label) && len + 1 < buf_size)
                    {
                        buf[len] = *c;
                        buf[len + 1] = '\0';
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            if (ImGui::Button("Space") && len + 1 < buf_size) { buf[len] = ' '; buf[len + 1] = '\0'; }
            ImGui::SameLine();
            if (ImGui::Button("Backspace") && len > 0) buf[len - 1] = '\0';
            ImGui::SameLine();
            if (ImGui::Button("Clear")) buf[0] = '\0';
            ImGui::SameLine();
            if (ImGui::Button("Done")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // ── Overlay-rendered markers ──────────────────────────────────────────
    // The world map is now drawn by the goblin::worldmap module (src/worldmap/):
    // map_renderer owns projection + motion-sync + group gating + draw; each marker
    // type is a MarkerLayer plugin (graces = 1st impl). This is the NEW overlay-
    // rendered map, distinct from the legacy native WorldMapPointParam injection.
    // Shared overlay marker layers: one GraceLayer (live BonfireWarp graces) + one
    // MapEntryLayer per other category (baked MAP_ENTRIES). Built once; layer order is
    // stable. Used by both the worldmap markers and the minimap HUD.
    std::vector<goblin::worldmap::MarkerLayer *> &overlay_layers()
    {
        namespace wm = goblin::worldmap;
        namespace gen = goblin::generated;
        static wm::GraceLayer s_graces;
        static wm::QuestNpcLayer s_quest_npc;
        static std::vector<wm::MapEntryLayer> s_cat;     // stable storage
        static std::vector<wm::MarkerLayer *> s_layers;  // pointers into the above
        if (s_layers.empty())
        {
            const int N = static_cast<int>(gen::Category::WorldLegacyDungeon) + 1;
            const int graces = static_cast<int>(gen::Category::WorldGraces);
            const int quest_npc = static_cast<int>(gen::Category::WorldQuestNPC);
            s_cat.reserve(N); // reserve → no realloc, so the pointers below stay valid
            for (int c = 0; c < N; ++c)
                if (c != graces &&     // graces come from the live GraceLayer, not MAP_ENTRIES
                    c != quest_npc)    // quest NPCs come from QuestNpcLayer, not MAP_ENTRIES
                    s_cat.emplace_back(c);
            s_layers.push_back(&s_graces);
            s_layers.push_back(&s_quest_npc);
            for (auto &L : s_cat)
                s_layers.push_back(&L);
        }
        return s_layers;
    }
}

    void goblin::overlay::draw_worldmap_markers(bool /*menu_open*/, const OverlayFrameCtx &ctx)
    {
        namespace wm = goblin::worldmap;
        if (!goblin::overlay_api::icons_enabled() && !wm::item_search_active())
            return; // master off (and no active search) → draw no overlay markers
        std::vector<wm::MarkerLayer *> &s_layers = overlay_layers();
        void *atlas = ctx.atlas_srv;
        // OS cursor in client/backbuffer px for marker tooltips (the map cursor tracks it).
        float mx = -1.f, my = -1.f;
        POINT pt{};
        BOOL ok = goblin::overlay_api::get_cursor_pos_real(&pt);
        if (ok && ctx.hwnd && ScreenToClient(ctx.hwnd, &pt)) { mx = (float)pt.x; my = (float)pt.y; }
        // Hand the renderer the harvested grace sprite (once ready) so it draws graces itself.
        if (ensure_grace_srv())
        {
            void *gs_gpu; ImVec2 gs_uv0, gs_uv1; int gs_nw, gs_nh;
            grace_srv_info(gs_gpu, gs_uv0, gs_uv1, gs_nw, gs_nh);
            wm::set_grace_sprite(gs_gpu, gs_uv0.x, gs_uv0.y, gs_uv1.x, gs_uv1.y, gs_nw, gs_nh);
        }
        if (ensure_grace_dungeon_srv())
        {
            void *gd_gpu; ImVec2 gd_uv0, gd_uv1;
            grace_dungeon_srv_info(gd_gpu, gd_uv0, gd_uv1);
            wm::set_grace_dungeon_sprite(gd_gpu, gd_uv0.x, gd_uv0.y, gd_uv1.x, gd_uv1.y);
        }
        wm::render_markers(s_layers, atlas, mx, my);
        // Item-search "locate": centre the live map on the located marker. The engine re-derives the
        // pan from the cursor reticle every frame (a Present-thread pan write is reverted), so the actual
        // centring is driven on the GAME thread inside the c32f0 step hook: set_locate_target() hands it
        // the marker, and the hook writes the reticle target each frame just before the engine's easer
        // pans toward it. HELD over a window of frames (the freeze re-asserts the static view), and the
        // nav jitter is kept alive so the c32f0 step keeps running with F1 open. set_view_center writes
        // the ZOOM-in + the debug snapshot.
        static float s_hold_u = 0.f, s_hold_v = 0.f;
        static int s_hold_frames = 0;
        static int s_settle_hits = 0;
        float lu = 0.f, lvv = 0.f;
        if (wm::take_locate_pos(&lu, &lvv)) { s_hold_u = lu; s_hold_v = lvv; s_hold_frames = 90; s_settle_hits = 0; }
        if (s_hold_frames > 0)
        {
            // Zoom in if the view is too far-out (else the item is a speck). kLocateZoom is calibration
            // (map zoom ~0.05..1.0; kGraceZoomRef in map_renderer is 0.25 "mid") — bump if still too wide.
            constexpr float kLocateZoom = 0.5f;
            goblin::overlay_api::set_view_center(s_hold_u, s_hold_v, kLocateZoom);
            goblin::overlay_api::set_locate_target(s_hold_u, s_hold_v); // game-thread c32f0 centres on it
            // Keep the map STEPPING (c32f0 runs) with F1 open: the per-frame step is gated on perceived
            // input, so keep the nav jitter alive for the whole hold.
            if (ctx.nav_frames->load(std::memory_order_relaxed) < s_hold_frames)
                ctx.nav_frames->store(s_hold_frames, std::memory_order_relaxed);
            --s_hold_frames;
            // EARLY RELEASE (perf): each hold-frame forces ER to step + re-composite its WHOLE Scaleform
            // world map — that engine cost (~tens of ms, NOT our ~0.1ms render) is the FPS drop after a
            // locate click. The 90-frame cap is only a fallback for a slow/far pan; the moment the live
            // view CONVERGES on the target (centre within a few map-units, 2 frames running to skip a
            // mid-ease false hit) we cut the hold to a short settle so the stepping stops early.
            constexpr int kSettle = 3;
            constexpr float kConvergeEps2 = 1024.f;  // ~32 marker-units off the screen centre
            goblin::worldmap_probe::LiveView lv2{};
            if (s_hold_frames > kSettle && goblin::overlay_api::get_live_view(lv2) && lv2.zoom > 0.f)
            {
                const float du = (lv2.panX + lv2.snapMidX) / lv2.zoom - s_hold_u;
                const float dv = (lv2.panZ + lv2.snapMidZ) / lv2.zoom - s_hold_v;
                if (du * du + dv * dv < kConvergeEps2)
                {
                    if (++s_settle_hits >= 2) s_hold_frames = kSettle;
                }
                else
                    s_settle_hits = 0;
            }
            if (s_hold_frames == 0)
                goblin::overlay_api::clear_locate_target(); // release the map (mouse pan resumes)
        }
    }

    // In-game minimap HUD (corner, north-up, overworld). Drawn during gameplay (map
    // closed) on the foreground draw list. No-ops internally when show_minimap is off,
    // the icons master is off, or the player is underground (pos not yet reliable).
    void goblin::overlay::draw_minimap_hud(const OverlayFrameCtx &ctx)
    {
        // Instrumented: the world-map close edge hands off to this minimap HUD, whose marker loop
        // does a read_event_flag() per marker. That first-closed-frame cost was previously unbenched,
        // so the "map-close lag" never showed up in the report or the spike warn. Now it does.
        GOBLIN_BENCH("render.minimap");
        void *atlas = ctx.atlas_srv;
        ImGuiIO &io = ImGui::GetIO();
        if (ensure_grace_srv())
        {
            void *gs_gpu; ImVec2 gs_uv0, gs_uv1; int gs_nw, gs_nh;
            grace_srv_info(gs_gpu, gs_uv0, gs_uv1, gs_nw, gs_nh);
            goblin::worldmap::set_grace_sprite(gs_gpu, gs_uv0.x, gs_uv0.y, gs_uv1.x, gs_uv1.y, gs_nw, gs_nh);
        }
        if (ensure_grace_dungeon_srv())
        {
            void *gd_gpu; ImVec2 gd_uv0, gd_uv1;
            grace_dungeon_srv_info(gd_gpu, gd_uv0, gd_uv1);
            goblin::worldmap::set_grace_dungeon_sprite(gd_gpu, gd_uv0.x, gd_uv0.y, gd_uv1.x, gd_uv1.y);
        }
        goblin::worldmap::draw_minimap(overlay_layers(), atlas, io.DisplaySize.x,
                                       io.DisplaySize.y);
    }

    void goblin::overlay::draw_panel(const OverlayFrameCtx &ctx)
    {
        ImGuiIO &io = ImGui::GetIO();

        // Disk-loot maps definitively not found (ancestor-walk AND the CreateFileW
        // observer came up empty within the timeout) → the disk source is REQUIRED
        // when loot_from_disk_msb is on, so replace the whole panel with a red error
        // instead of drawing an empty/misleading map.
        if (goblin::overlay_api::disk_loot_state() == goblin::worldmap::DiskLootState::Failed)
        {
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::Begin("Map for Goblins##error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
            ImGui::TextUnformatted("Map for Goblins - ERROR");
            ImGui::Separator();
            ImGui::TextWrapped("The mod's map folder (map\\MapStudio\\*.msb.dcx) could not be "
                               "found. The mod cannot load its markers.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            std::string sd = goblin::overlay_api::disk_loot_dir().string();
            if (!sd.empty())
                ImGui::TextDisabled("Last path searched: %s", sd.c_str());
            ImGui::TextDisabled("Set 'loot_msb_dir' in MapForGoblins.ini to your mod's");
            ImGui::TextDisabled("map\\MapStudio folder (the markers are read from it live).");
            ImGui::End();
            return;
        }

        // One or more RE signatures (AOB) failed to resolve uniquely at init → the mod
        // hooked the wrong function or nothing at all, so markers/graces/loot will be
        // wrong or absent. Surface it instead of silently rendering a broken map (the
        // [SIG] log is invisible mid-game). MULTI = ambiguous (likely-wrong function),
        // FAIL = gone (needs re-find after a game update).
        {
            const goblin::sig::SigHealth &sh = goblin::sig::sig_health();
            if (sh.ran && (sh.fail > 0 || sh.multi > 0))
            {
                ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
                ImGui::Begin("Map for Goblins##sigerror", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.15f, 1.0f));
                ImGui::TextUnformatted("Map for Goblins - WARNING");
                ImGui::Separator();
                ImGui::TextWrapped("The RE signatures were not resolved correctly. The mod "
                                   "will probably not work correctly (markers, graces or "
                                   "loot missing or wrong).");
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::TextDisabled("%d missing, %d ambiguous out of %d signatures.",
                                    sh.fail, sh.multi, sh.total);
                ImGui::TextDisabled("Likely broken by a game update — see the [SIG] log");
                ImGui::TextDisabled("for details (the AOBs need to be re-found).");
                ImGui::End();
                return;
            }
        }

        if (!g_large)
        {
            // Compact widget: a small corner pill that expands to the full panel.
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);  // auto-fit
            ImGui::Begin("Map for Goblins##small", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
            if (ImGui::Button("Map for Goblins  [+]"))  // expand
                g_large = true;
            ImGui::SameLine();
            ImGui::TextDisabled("F1");
            ImGui::End();
        }
        else
        {
            // Full panel — live controls (post intents to the watcher thread).
            // Auto-fit to content (so it stops being clipped too small), clamped
            // to a sane min/max so it neither shrinks to nothing nor overflows the
            // screen; if content exceeds the max it scrolls.
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(360.0f, 240.0f),
                ImVec2(720.0f, io.DisplaySize.y * 0.92f));
            ImGui::Begin("Map for Goblins##large", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            // Keep the per-category census warm: the watcher only runs the flag
            // sweep while the panel is on-screen (stamped here each frame).
            goblin::overlay_api::note_menu_visible();
            if (ImGui::Button("[-] collapse"))
                g_large = false;
            ImGui::SameLine();
            ImGui::TextDisabled("F1 close | %.0f fps", io.Framerate);
            ImGui::Separator();

            // Settings search: typing here auto-expands every top-level section whose title
            // matches (and force-collapses the rest) so a setting buried in a rarely-opened
            // section (e.g. "Minimap") can be found without manually clicking through each
            // header. Separate from the "Sections & categories" search below, which filters
            // marker CATEGORIES, not panel settings sections.
            static ImGuiTextFilter s_settings_filter;
            s_settings_filter.Draw("Find setting (section name)##settingsfilter", ImGui::GetContentRegionAvail().x);
            const bool settings_filtering = s_settings_filter.IsActive();
            ImGui::Separator();

            // P2b vertical slice: draw live-harvested item icons (copied GPU→GPU from the engine's
            // menu sheets). Open the inventory first to harvest, then check here. Dev-gated.
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Item icons (P2b test)"), ImGuiCond_Always);
            if ((*goblin::overlay_api::cfg_dumpIconTextures_ptr()) && ImGui::CollapsingHeader("Item icons (P2b test)"))
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
                            DXGI_FORMAT f;
                            s_ram_tex = create_tex_from_dds_mem(dds.data(), dds.size(), s_ram_w, s_ram_h, f);
                            s_ram_fmt = (int)f;
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
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Icon migration (Baked -> GPU)"), ImGuiCond_Always);
            if (ImGui::CollapsingHeader("Icon migration (Baked \xE2\x86\x92 GPU)"))
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
                const bool grace_gpu = native_on && (*goblin::overlay_api::cfg_graceOverlay_ptr()) && (*goblin::overlay_api::cfg_graceGpuSprite_ptr());
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
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("ERR map sprites (GPU debug)"), ImGuiCond_Always);
            if ((*goblin::overlay_api::cfg_dumpIconTextures_ptr()) && ImGui::CollapsingHeader("ERR map sprites (GPU debug)"))
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
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Grace texture debug (live)"), ImGuiCond_Always);
            if ((*goblin::overlay_api::cfg_dumpIconTextures_ptr()) && ImGui::CollapsingHeader("Grace texture debug (live)"))
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

            // Master on/off + Save.
            bool icons_on = goblin::overlay_api::icons_enabled();
            if (ImGui::Checkbox("Show icons (master)", &icons_on))
                goblin::overlay_api::set_icons_enabled(icons_on);
            ImGui::SameLine();
            static double saved_at = -10.0;
            if (ImGui::Button("Save to INI"))
            {
                goblin::overlay_api::request_save();
                saved_at = ImGui::GetTime();
            }
            // Brief confirmation so the button isn't a silent no-op (the file I/O
            // happens on the watcher thread; this just acknowledges the click).
            double since = ImGui::GetTime() - saved_at;
            if (since < 2.0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Saved to INI");
            }

            // Map-fragment gate (live; persists via "Save to INI"). When on, a marker stays hidden
            // until the player has acquired that area's map-fragment ITEM (fragment event flag).
            ImGui::Checkbox("Require map fragments (hide an area's icons until its fragment is found)",
                            goblin::overlay_api::cfg_requireMapFragments_ptr());

            // DIAG: draw ONLY the no-bake residual (Baked-source markers; disk/live twins already
            // deduped away). Fly the world + eyeball each spot — real loot the live pass misses
            // (coverage gap) vs a phantom the bake invented (bake bug). See nobake_scoreboard.md.
            ImGui::Checkbox("Baked-only (diag: show just the no-bake residual)",
                            goblin::overlay_api::cfg_bakedOnly_ptr());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hides every marker the live disk/memory passes already cover,\n"
                                  "leaving only the markers still coming from the static bake.\n"
                                  "Use it to judge each leftover: real loot we fail to source live\n"
                                  "(coverage miss) vs a stale/invented spot (bake bug).");

            // Collected/cleared graying (overlay map; live, persists via "Save to INI").
            // On = dim looted items / killed bosses (cleared bosses get a checkmark);
            // hide_collected switches dim → remove (legacy native-map behaviour).
            ImGui::Checkbox("Gray collected/cleared markers (dim looted items & killed bosses)",
                            goblin::overlay_api::cfg_collectedGraying_ptr());
            if ((*goblin::overlay_api::cfg_collectedGraying_ptr()))
            {
                ImGui::SameLine();
                ImGui::Checkbox("hide instead", goblin::overlay_api::cfg_hideCollected_ptr());
            }

            // Merge co-located identical-item loot markers into one "xN". Pure render decision (the
            // grouping is annotated once at build) → instant toggle, no bucket rebuild.
            ImGui::Checkbox("Stack identical items (merge same-item nodes within ~5m)",
                            goblin::overlay_api::cfg_stackIdenticalItems_ptr());

            // Major-region name labels (overlay map; live, persists via "Save to INI").
            ImGui::Checkbox("Show region labels (major-region names on the map)",
                            goblin::overlay_api::cfg_showRegionLabels_ptr());

            // Redify boss markers (overlay port of the legacy red-skull iconId; live,
            // persists via "Save to INI"). Tints WorldBosses markers red (overworld +
            // dungeon bosses); collected/cleared graying still takes precedence.
            ImGui::Checkbox("Red boss markers (tint boss icons red)",
                            goblin::overlay_api::cfg_redifyBossIcons_ptr());

            // DX item 7: up/down altitude badge for markers above/below the player (player's map only).
            ImGui::Checkbox("Altitude arrows (up = above / down = below player)",
                            goblin::overlay_api::cfg_altitudeCue_ptr());

            // Grace rendering: overlay draws all graces (discovered=colour, undiscovered=grey).
            ImGui::Checkbox("Overlay graces (draw all graces ourselves)",
                            goblin::overlay_api::cfg_graceOverlay_ptr());
            if ((*goblin::overlay_api::cfg_graceOverlay_ptr()))
            {
                ImGui::SameLine();
                ImGui::Checkbox("GPU sprite (engine, time-tinted) vs CPU (baked atlas)",
                                goblin::overlay_api::cfg_graceGpuSprite_ptr());
            }

            // Spoiler-free loot (overlay port of anonymous_loot; live, persists via
            // "Save to INI"). Lot-backed loot markers draw as a gray "?" with a generic
            // label instead of the real item icon/name.
            ImGui::Checkbox("Spoiler-free loot (gray \"?\" instead of the item)",
                            goblin::overlay_api::cfg_anonymousLoot_ptr());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every loot marker shows a gray \"?\" and only its location,\n"
                                  "hiding the real item (useful with randomizers). Markers still\n"
                                  "gray out when collected; category show/hide is unaffected.");

            // Gamepad overlay-toggle combo (dx-bugs-backlog PR C item 3). Recorder arms the
            // XInput poll in hk_present; first nonzero button read there wins and saves.
            ImGui::Text("Gamepad toggle combo: %s",
                        goblin::overlay_api::mask_to_combo_string((*goblin::overlay_api::cfg_overlayToggleGamepad_ptr())).c_str());
            ImGui::SameLine();
            if (*ctx.gamepad_combo_recording)
            {
                // Two phases: first wait for the button that ARMED recording (e.g. gamepad-nav A
                // on this very widget) to fully release, THEN start listening — otherwise that
                // same activating press gets captured as the whole combo.
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                    *ctx.gamepad_combo_ready ? "Press buttons now…" : "Release all buttons…");
            }
            else if (ImGui::SmallButton("Record gamepad combo"))
            {
                *ctx.gamepad_combo_recording = true;
                ctx.gamepad_combo_reject_reason->clear();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click, then press the button combo on your controller (default Y+R3).\n"
                                  "The first combo read is captured and saved to the ini immediately.");
            if (!ctx.gamepad_combo_reject_reason->empty())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", ctx.gamepad_combo_reject_reason->c_str());

            // On-screen keyboard layout (dx-bugs-backlog PR C-2 part 2). Like every other plain
            // setting here, this only persists via "Save to INI" below — no immediate auto-save.
            ImGui::Text("Gamepad keyboard layout:");
            ImGui::SameLine();
            int kbd_layout = (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr());
            if (ImGui::RadioButton("Alphabetical", &kbd_layout, 0))
                (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) = static_cast<uint8_t>(kbd_layout);
            ImGui::SameLine();
            if (ImGui::RadioButton("QWERTY", &kbd_layout, 1))
                (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) = static_cast<uint8_t>(kbd_layout);

            // Overlay marker scale (live preview; persists via "Save to INI"). Final
            // size = resolution-relative base × master × per-type scale.
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Marker scale (overlay map)"), ImGuiCond_Always);
            if (ImGui::CollapsingHeader("Marker scale (overlay map)"))
            {
                scale_control("Master", goblin::overlay_api::cfg_overlayMasterScale_ptr(), 0.3f, 3.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Category icons", goblin::overlay_api::cfg_overlayIconScale_ptr(), 0.3f, 10.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Grace icons (calib)", goblin::overlay_api::cfg_graceIconScale_ptr(), 0.2f, 10.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Grace offset X (native vs imgui)", goblin::overlay_api::cfg_graceOffsetX_ptr(), -200.0f, 200.0f, 1.0f, 10.0f, "%.0f");
                scale_control("Grace offset Y (native vs imgui)", goblin::overlay_api::cfg_graceOffsetY_ptr(), -200.0f, 200.0f, 1.0f, 10.0f, "%.0f");
                scale_control("Cluster piles", goblin::overlay_api::cfg_overlayClusterScale_ptr(), 0.3f, 3.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Marker motion delay (frames)", goblin::overlay_api::cfg_viewDelayFrames_ptr(), 0.0f, 7.0f, 0.1f, 0.5f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Project markers this many present-frames behind the eased basemap.\n"
                                      "Pan the map: raise if markers LEAD (snap back on stop), lower if they TRAIL.\n"
                                      "1.0 = default. Tune to kill the pan/zoom re-adjust, then Save to INI.");
                ImGui::Checkbox("Delay zoom too", goblin::overlay_api::cfg_viewDelayZoom_ptr());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON (default): the motion delay applies to zoom as well as pan.\n"
                                      "If markers TELEPORT for one frame on each mouse-wheel zoom step,\n"
                                      "turn this OFF — markers then use the live zoom while still delaying pan.");
                if (ImGui::SmallButton("Reset##scale"))
                {
                    (*goblin::overlay_api::cfg_overlayMasterScale_ptr()) = 1.0f;
                    (*goblin::overlay_api::cfg_overlayIconScale_ptr()) = 1.2f;     // match the schema defaults
                    (*goblin::overlay_api::cfg_overlayClusterScale_ptr()) = 1.0f;
                    (*goblin::overlay_api::cfg_graceIconScale_ptr()) = 1.2f;
                    (*goblin::overlay_api::cfg_graceOffsetX_ptr()) = 0.0f;
                    (*goblin::overlay_api::cfg_graceOffsetY_ptr()) = 0.0f;
                    (*goblin::overlay_api::cfg_viewDelayFrames_ptr()) = 1.0f;
                    (*goblin::overlay_api::cfg_viewDelayZoom_ptr()) = true;
                }
                ImGui::TextDisabled("Slider = coarse; type in the box or use its +/- arrows for an exact\n"
                                    "value (Ctrl+Click the slider also types). × a resolution-relative\n"
                                    "base. Save to INI to persist.");
            }

            // In-game minimap HUD (foundation; overworld-only, north-up). Live; persists.
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Minimap (in-game HUD)"), ImGuiCond_Always);
            if (ImGui::CollapsingHeader("Minimap (in-game HUD)"))
            {
                ImGui::Checkbox("Show minimap (corner HUD during gameplay)",
                                goblin::overlay_api::cfg_showMinimap_ptr());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A small north-up minimap in the screen corner showing nearby\n"
                                      "markers around you during play. OVERWORLD only for now\n"
                                      "(underground player position isn't reliable yet).");
                // Max raised 0.30 -> 0.60 (user feedback 2026-07-01: 0.30 was still too
                // zoomed-out/small at max). Default also raised, see minimapZoom's declaration.
                // AlwaysClamp: ImGui's Ctrl+Click-to-type on a slider does NOT clamp to
                // [min,max] by default -- a typed value beyond what's shown could be saved to
                // the INI, then silently reset on the next load by the (now correct, but still
                // real) per-field range clamp in goblin_config.cpp. Keep what's shown and what's
                // stored always in sync.
                ImGui::SliderFloat("Zoom (px/world)", goblin::overlay_api::cfg_minimapZoom_ptr(), 0.02f, 5.0f, "%.3f",
                                   ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Radius (px)", goblin::overlay_api::cfg_minimapSize_ptr(), 60.0f, 300.0f, "%.0f",
                                   ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Opacity", goblin::overlay_api::cfg_minimapOpacity_ptr(), 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Anchor right", goblin::overlay_api::cfg_minimapAnchorRight_ptr());
                ImGui::SameLine();
                ImGui::Checkbox("Anchor bottom", goblin::overlay_api::cfg_minimapAnchorBottom_ptr());
                ImGui::SliderFloat("Offset X", goblin::overlay_api::cfg_minimapOffsetX_ptr(), 0.0f, 600.0f, "%.0f");
                ImGui::SliderFloat("Offset Y", goblin::overlay_api::cfg_minimapOffsetY_ptr(), 0.0f, 600.0f, "%.0f");
                ImGui::TextDisabled("North-up. Hidden while the world map is open. Save to INI to persist.");
            }

            // ── Find item / object ────────────────────────────────────────────────────────────
            // Search markers by NAME: type a fragment, get a results list; the matching markers are
            // ringed on the map (and pulled out of cluster piles). Click a result to point the map
            // cursor at it (cursor = the 2D map camera). The match set is rebuilt only when the query
            // changes (resolving ~thousands of FMG names per frame would be too costly), so the hot
            // render loop just does an O(1) name_id set lookup.
            ImGui::SeparatorText("Find item / object");
            {
                // group bits (marker_layer): bit0 = underground, bit1 = DLC. Label the page so the
                // user knows where a hit is — locate only pans WITHIN the open page (cross-page needs
                // a manual page switch first; auto-switch would need page-transition RE).
                auto page_label = [](int g) -> const char * {
                    switch (g & 3) { case 1: return "Underground"; case 2: return "DLC";
                                     case 3: return "DLC Underground"; default: return "Overworld"; }
                };
                // One result row per (name, PAGE): an item on several pages (e.g. a Larval Tear on
                // Overworld + Underground + DLC) gets a separate row per page, so clicking a row locates
                // on THAT page. (Deduping by name_id alone collapsed them into one row carrying only the
                // first marker's group — the "shows only Underground" bug.)
                struct Hit { std::string label; int32_t name_id; int count; int group; bool quest = false; };
                static char item_q[64] = "";
                static std::string s_last_q;
                static std::unordered_set<int32_t> s_match;   // name_ids whose name matches (rendered ring)
                static std::vector<Hit> s_hits;               // deduped results for the list
                static int32_t s_pending_locate = 0;
                static std::string s_locate_label;            // clicked item name (pending banner)
                static int s_locate_group = 0;                // clicked item page (pending banner)

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::InputTextWithHint("##itemsearch", "find item/object by name... (e.g. larval, bolt of)",
                                         item_q, sizeof(item_q));
                draw_gamepad_keyboard_button("##itemsearch_kbd", item_q, sizeof(item_q));

                // ── Locate debug (dev) — gated behind Verbose logging (debug_logging). Diagnoses the
                // item-search centring: LIVE view vs the target the engine should ease the pan toward
                // ("live-vs-target dPan" → CENTERED OK once converged). Kept for future locate issues.
                if ((*goblin::overlay_api::cfg_debugLogging_ptr()) && ImGui::TreeNode("Locate debug (dev)"))
                {
                    goblin::worldmap_probe::LiveView dlv{};
                    const bool dbg_open = goblin::overlay_api::get_live_view(dlv);
                    const auto &d = goblin::overlay_api::last_locate_debug();
                    if (dbg_open && dlv.zoom != 0.f)
                    {
                        const float cu = (dlv.panX + dlv.snapMidX) / dlv.zoom;
                        const float cv = (dlv.panZ + dlv.snapMidZ) / dlv.zoom;
                        ImGui::Text("LIVE pan=(%.1f, %.1f) zoom=%.3f  centre(marker)=(%.1f, %.1f)",
                                    dlv.panX, dlv.panZ, dlv.zoom, cu, cv);
                        ImGui::Text("LIVE snapMid=(%.1f, %.1f)", dlv.snapMidX, dlv.snapMidZ);
                    }
                    else
                        ImGui::TextDisabled("map closed / no live view (open the world map)");
                    ImGui::Separator();
                    if (!d.ran)
                        ImGui::TextDisabled("no locate yet — click a result");
                    else
                    {
                        ImGui::Text("cursorOk=%d  wrote=%d  rectOk=%d", d.cursorOk, d.wrote, d.rectOk);
                        ImGui::Text("req centre   = (%.1f, %.1f)", d.reqU, d.reqV);
                        ImGui::TextColored(d.clamped ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                                           "clamp centre = (%.1f, %.1f)  %s", d.clampU, d.clampV,
                                           d.clamped ? "[CLAMPED -> near edge]" : "[no clamp]");
                        ImGui::Text("zoom %.3f -> %.3f   world=[%.0f,%.0f .. %.0f,%.0f]",
                                    d.zoomBefore, d.zoomUsed, d.wMinX, d.wMinZ, d.wMaxX, d.wMaxZ);
                        ImGui::Text("pan TARGET   = (%.1f, %.1f)  (driven via cursor, not written)",
                                    d.panWroteX, d.panWroteZ);
                        if (dbg_open)
                        {
                            const float dx = dlv.panX - d.panWroteX, dz = dlv.panZ - d.panWroteZ;
                            const bool off = (dx > 8.f || dx < -8.f || dz > 8.f || dz < -8.f);
                            ImGui::TextColored(off ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.3f, 1, 0.3f, 1),
                                               "live-vs-target dPan=(%.1f, %.1f)  %s", dx, dz,
                                               off ? "<- converging / not there yet" : "<- CENTERED OK");
                        }
                    }
                    ImGui::TreePop();
                }

                if (s_last_q != item_q)
                {
                    s_last_q = item_q;
                    s_match.clear();
                    s_hits.clear();
                    if (item_q[0] != '\0')
                    {
                        // Resolve each distinct name_id once (cache), substring-match the query
                        // against BOTH the live (game-language) label AND the bundled English
                        // alias — so a player on a French/other game can type the English/wiki
                        // name and still find it. The displayed label stays the game-language
                        // name, with the English in parens when it differs (e.g. on a non-EN
                        // game) to confirm the match.
                        struct Names { std::string loc, en, label; };
                        std::unordered_map<int32_t, Names> name_cache;
                        // Dedup result rows by (name_id, page) via a composite key (name_id<<2 | group),
                        // so each page an item is on becomes its own row. s_match stays keyed by name_id
                        // so EVERY instance still highlights/rings on the map.
                        std::map<int64_t, int> hit_count;  // (name_id<<2 | group) -> marker count
                        // Keys (name<<2|page) that have at least ONE quest-NPC marker → badge the row
                        // "[quest]" so the player can tell which search hit is a quest pin on the map
                        // without clicking each one. OR'd across all markers of the key (a name_id could
                        // appear as both a quest pin and something else).
                        std::unordered_set<int64_t> quest_keys;
                        const int questCat = static_cast<int>(goblin::generated::Category::WorldQuestNPC);
                        for (auto *L : overlay_layers())
                        {
                            if (!L) continue;
                            for (const auto &m : L->markers())
                            {
                                if (m.name_id < 0) continue;
                                auto it = name_cache.find(m.name_id);
                                if (it == name_cache.end())
                                {
                                    Names n;
                                    n.loc = goblin::overlay_api::lookup_text_utf8(m.name_id);
                                    // English alias resolved live from the active install's engus
                                    // FMGs on disk (mod-agnostic; empty if unavailable → search
                                    // degrades to game-language matching, no wrong-mod names).
                                    n.en = goblin::overlay_api::lookup_name_en_disk_utf8(m.name_id);
                                    // Label = game-language name; fall back to English if the
                                    // live FMG had no entry. Append "(English)" only when it adds
                                    // information (present and different from the shown name).
                                    n.label = n.loc.empty() ? n.en : n.loc;
                                    if (!n.en.empty() && n.en != n.label)
                                        n.label += " (" + n.en + ")";
                                    it = name_cache.emplace(m.name_id, std::move(n)).first;
                                }
                                const Names &nm = it->second;
                                if (nm.label.empty()) continue;
                                // Word-order-independent: each query token must appear in the
                                // combined game-language + English text (so "Claw Talisman" and
                                // "Talisman Claw" both match, and FR/EN words can be mixed).
                                if (!matches_all_tokens(nm.loc + " " + nm.en, item_q)) continue;
                                const int g = m.group & 3;
                                s_match.insert(m.name_id);                       // ring every instance
                                const int64_t k = ((int64_t)m.name_id << 2) | g; // per (name, page)
                                // One marker = one instance. Item stacking is now a non-destructive
                                // RENDER annotation (every co-located node stays a real marker in the
                                // bucket, reps + members alike), so a plain +1 per marker already gives
                                // the true on-map count (4 Formic Rock nodes read 4) regardless of the
                                // stack toggle — no per-stack adjustment needed.
                                const bool first = (hit_count[k] == 0);
                                hit_count[k] += 1;
                                if (m.category == questCat) quest_keys.insert(k);
                                if (first)
                                    s_hits.push_back({nm.label, m.name_id, 0, g});
                            }
                        }
                        for (auto &h : s_hits)
                        {
                            const int64_t k = ((int64_t)h.name_id << 2) | h.group;
                            h.count = hit_count[k];
                            h.quest = quest_keys.count(k) != 0;
                        }
                        // Group same-name rows together, pages in order (Overworld, Underground, DLC).
                        std::sort(s_hits.begin(), s_hits.end(), [](const Hit &a, const Hit &b) {
                            int c = a.label.compare(b.label);
                            return c != 0 ? c < 0 : a.group < b.group;
                        });
                    }
                }

                if (item_q[0] != '\0')
                {
                    // Locate (pan / page-switch) needs the world map OPEN — the live view + the
                    // game-thread switch step only exist then. When closed, the list stays browsable
                    // (what exists + which region) but the rows are disabled with a hint, so a click
                    // can't leave a locate dangling forever.
                    goblin::worldmap_probe::LiveView lv{};
                    const bool map_open = goblin::overlay_api::get_live_view(lv);
                    const int open_grp = map_open ? ((lv.openDlc ? 2 : 0) | (lv.underground ? 1 : 0)) : 0;

                    if (map_open)
                        ImGui::TextDisabled("%zu match%s (ringed on map; click = pan map onto it)",
                                            s_hits.size(), s_hits.size() == 1 ? "" : "es");
                    else if ((*goblin::overlay_api::cfg_showMinimap_ptr()))
                        // <user> 2026-07-01: this used to always say "open the world map to
                        // locate them" even with the minimap on — wrong, the minimap already
                        // rings a hit (including off-range ones, clamped to the HUD edge).
                        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f),
                                           "%zu match%s (ringed on the minimap)",
                                           s_hits.size(), s_hits.size() == 1 ? "" : "es");
                    else
                        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                                           "%zu match%s - open the world map to locate them",
                                           s_hits.size(), s_hits.size() == 1 ? "" : "es");

                    // Region-visited gate from GRACE DISCOVERY (robust, save-backed — supersedes the
                    // fragile dialog availability byte): if NO grace in a region has been rested at, the
                    // player has never been there, so its map is undiscovered — grey those results +
                    // don't teleport in. Covers DLC (bit1) AND underground (bit0). bit1=DLC, bit0=UG.
                    // Recomputed LIVE every throttled tick (NOT latched) so DISCOVERING a new grace mid-
                    // session unlocks its page within ~0.5s, and a save/character switch re-locks it.
                    // There is NO reliable native O(1) "page discovered" flag (the dialog DLC byte
                    // read unreliably; the UG flag was never found — see goblin_worldmap_probe.hpp
                    // TODO(page_og_underground_available)). read_event_flag() IS O(1), though: harvest
                    // the grace discover-flags per page group ONCE (the flag IDs are save-independent —
                    // every grace ROW exists in BonfireWarpParam regardless of discovery), then each
                    // throttled tick read only that small list.
                    // DO NOT LATCH seen=true: these statics outlive the DLL, so a save/character switch
                    // within one game session (graces un-discover) would keep a latched page wrongly
                    // unlocked — the "I rested at no grace yet nothing blocks" bug. We recompute seen
                    // LIVE every tick instead (the list is tiny, so it's free).
                    static bool s_dlc_seen = false, s_ug_seen = false;
                    static std::vector<uint32_t> s_dlc_grace_flags, s_ug_grace_flags;
                    static bool s_grace_flags_built = false;
                    static int s_visit_tick = 0;
                    if (map_open && (++s_visit_tick % 30 == 0))
                    {
                        // Only fires during an active search (map open, 1 tick in 30). The first run
                        // also builds the flag lists (one ~8477-marker pass); steady-state is just a
                        // few-dozen flag reads — the bench line shows both.
                        GOBLIN_BENCH("overlay.item_search.gate_scan");
                        if (!s_grace_flags_built)
                        {
                            for (auto *L : overlay_layers())
                            {
                                if (!L) continue;
                                for (const auto &m : L->markers())
                                {
                                    if (!m.discover_flag) continue;  // graces only carry a discover flag
                                    if (m.group & 2) s_dlc_grace_flags.push_back((uint32_t)m.discover_flag);
                                    if (m.group & 1) s_ug_grace_flags.push_back((uint32_t)m.discover_flag);
                                }
                            }
                            s_grace_flags_built = true;
                        }
                        s_dlc_seen = false;
                        for (uint32_t f : s_dlc_grace_flags)
                            if (goblin::overlay_api::read_event_flag(f)) { s_dlc_seen = true; break; }
                        s_ug_seen = false;
                        for (uint32_t f : s_ug_grace_flags)
                            if (goblin::overlay_api::read_event_flag(f)) { s_ug_seen = true; break; }
                    }
                    if (ImGui::BeginChild("##itemhits", ImVec2(0, 150), true))
                    {
                        if (!map_open) ImGui::BeginDisabled();
                        for (size_t i = 0; i < s_hits.size(); i++)
                        {
                            const Hit &h = s_hits[i];
                            const bool off_page = (h.group & 3) != (open_grp & 3);
                            // Locked = this row's page is a region the player hasn't visited (no grace).
                            const bool locked = map_open && (((h.group & 2) && !s_dlc_seen) ||
                                                             ((h.group & 1) && !s_ug_seen));
                            char row[200];
                            std::snprintf(row, sizeof(row), "%s  (x%d) - %s%s%s##h%zu", h.label.c_str(),
                                          h.count, page_label(h.group),
                                          h.quest ? " [quest]" : "",
                                          locked ? " [undiscovered]" : "", i);
                            if (locked) ImGui::BeginDisabled();
                            if (ImGui::Selectable(row) && map_open)
                            {
                                s_pending_locate = h.name_id;  // click → pan the map onto it
                                s_locate_label = h.label;      // remembered for the pending banner
                                s_locate_group = h.group;      // this row's page
                                ctx.nav_frames->store(90, std::memory_order_relaxed);  // wake the map so
                                                  // the switch+pan apply with the F1 panel still open
                                // Cross-page: switch to this row's page+layer (overworld<->DLC +
                                // surface<->UG), marshalled onto the game thread, then the locate pans.
                                if (off_page)
                                    goblin::overlay_api::request_switch_to_page(h.group);
                            }
                            if (locked) ImGui::EndDisabled();
                            if (map_open && !locked && off_page && ImGui::IsItemHovered())
                                ImGui::SetTooltip("On the %s map — click to switch there + centre on it.",
                                                  page_label(h.group));
                            if (locked && ImGui::IsItemHovered())
                                ImGui::SetTooltip("On the %s map — you haven't discovered it yet.",
                                                  page_label(h.group));
                        }
                        if (!map_open) ImGui::EndDisabled();
                        if (s_hits.empty())
                            ImGui::TextDisabled("no marker matches");
                    }
                    ImGui::EndChild();

                    // Cross-page locate: the switch is marshalled to the game thread + the locate pans
                    // the instant that page opens. The banner shows until it lands.
                    if (map_open && goblin::worldmap::locate_pending())
                        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                                           "> Locating \"%s\" on the %s map...", s_locate_label.c_str(),
                                           page_label(s_locate_group));
                }
                // Hand the renderer the live match set + any pending locate (consumed once).
                goblin::worldmap::set_item_search(item_q[0] ? &s_match : nullptr, s_pending_locate);
                s_pending_locate = 0;
            }

            // Sections (coarse) + their categories (fine). A row shows only if
            // both its section and its category are enabled.
            ImGui::SeparatorText("Sections & categories");
            // Search box: filter the category list by name. Matching a SECTION name
            // shows that whole section; otherwise only matching category rows show,
            // and sections with no match are hidden. Sections auto-expand while
            // filtering so matches are visible without manual clicking.
            static char cat_filter[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##catfilter", "search categories... (e.g. sorcer, ash, smith)",
                                     cat_filter, sizeof(cat_filter));
            draw_gamepad_keyboard_button("##catfilter_kbd", cat_filter, sizeof(cat_filter));
            const bool cat_filtering = cat_filter[0] != '\0';
            for (int s = 0; s < goblin::overlay_api::section_count(); s++)
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

                for (int c = 0; c < goblin::overlay_api::category_count(); c++)
                {
                    if (goblin::overlay_api::category_section(c) != s) continue;
                    if (!show_all_cats && !contains_ci(goblin::overlay_api::category_label(c), cat_filter))
                        continue;   // search box: hide non-matching category rows
                    ImGui::PushID(c);
                    const char *clabel = goblin::overlay_api::category_label(c);
                    bool cv = goblin::overlay_api::category_visible(c);
                    if (ImGui::Checkbox(clabel, &cv))
                        goblin::overlay_api::set_category_visible(c, cv);
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

            ImGui::SeparatorText("ERR integration");
            {
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

            ImGui::SeparatorText("Quest navigation");
            {
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

            ImGui::SeparatorText("Clustering");
            {
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

            // Dev/debug observers. Each installs a hook or starts a worker thread
            // ONCE at startup based on its config flag (dllmain setup), so these
            // are restart-required: flip + Save, then relaunch. save_all_bool_settings
            // persists every config bool, so no per-flag plumbing is needed.
            ImGui::SeparatorText("Debug");
            // Live toggle (no restart): project_marker reads this every frame.
            ImGui::Checkbox("Live projection (engine world→map fn; fixes dungeon/UG placement)",
                            goblin::overlay_api::cfg_liveProjection_ptr());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Project markers with the game's OWN WorldMapViewModel instead of the baked\n"
                    "affine. Toggles live (open the map and flip it to compare). Underground /\n"
                    "dungeon markers snap to their real LegacyConv-folded positions. Falls back\n"
                    "to baked until the map is open (engine VM must resolve).");
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Dev tools (Save + restart)"), ImGuiCond_Always);
            if (ImGui::TreeNode("Dev tools (Save + restart)"))
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
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.22f, 0.22f, 1.0f));
            if (settings_filtering) ImGui::SetNextItemOpen(s_settings_filter.PassFilter("Danger zone"), ImGuiCond_Always);
            if (ImGui::TreeNode("Danger zone"))
            {
                if (ImGui::Button("Reset quest progression"))
                    ImGui::OpenPopup("##confirm_reset_quest");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clear every quest-step checkmark. Save to INI to persist.");
                if (ImGui::Button("Reset parameters to default"))
                    ImGui::OpenPopup("##confirm_reset_params");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Restore all settings to defaults and write the ini.\n"
                                      "Restart the game to fully apply.");

                if (ImGui::BeginPopupModal("##confirm_reset_quest", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted("Clear ALL quest-step checkmarks?");
                    ImGui::TextDisabled("Cannot be undone. Save to INI afterwards to persist.");
                    if (ImGui::Button("Yes, clear"))
                    {
                        goblin::overlay_api::reset_quest_progress();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (ImGui::BeginPopupModal("##confirm_reset_params", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted("Reset ALL settings to defaults?");
                    ImGui::TextDisabled("Writes defaults to MapForGoblins.ini. Restart to fully apply.");
                    if (ImGui::Button("Yes, reset"))
                    {
                        goblin::overlay_api::reset_to_defaults();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::TreePop();
            }
            ImGui::PopStyleColor(3);

            ImGui::End();
        }
    }
