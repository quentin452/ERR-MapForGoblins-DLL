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
#include "goblin_pause.hpp"              // F1 "Pause the game" checkbox (host-resolved branch)
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

// The panel-section files (src/overlay_panel/) own the text-matching helpers, the
// shared widgets and each section's UI — see panel_internal.hpp. This file keeps the
// window frame, the two draw entry points, and the shared marker layers below.
#include "overlay_panel/panel_internal.hpp"
#include "goblin_i18n.hpp"
using goblin::i18n::tr;  // overlay UI localization

namespace
{
    bool g_large = true;         // false = compact widget, true = full panel
}

    // ── Overlay-rendered markers ──────────────────────────────────────────
    // The world map is now drawn by the goblin::worldmap module (src/worldmap/):
    // map_renderer owns projection + motion-sync + group gating + draw; each marker
    // type is a MarkerLayer plugin (graces = 1st impl). This is the NEW overlay-
    // rendered map, distinct from the legacy native WorldMapPointParam injection.
    // Shared overlay marker layers: one GraceLayer (live BonfireWarp graces) + one
    // MapEntryLayer per other category (baked MAP_ENTRIES). Built once; layer order is
    // stable. Used by both the worldmap markers and the minimap HUD — and by the F1
    // item search (src/overlay_panel/panel_search.cpp), hence externally linkable.
    std::vector<goblin::worldmap::MarkerLayer *> &goblin::overlay::overlay_layers()
    {
        namespace wm = goblin::worldmap;
        namespace gen = goblin::generated;
        static wm::GraceLayer s_graces;
        static wm::QuestNpcLayer s_quest_npc;
        static std::vector<wm::MapEntryLayer> s_cat;     // stable storage
        static std::vector<wm::MarkerLayer *> s_layers;  // pointers into the above
        if (s_layers.empty())
        {
            const int N = static_cast<int>(gen::Category::WorldFarmableCollectible) + 1;
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
            // EXCEPT when the engine is clamping this target (deep-fog locate, F2): the c32f0
            // detour then writes the target pan directly post-step, and the only thing that
            // claws it back is the easer — which runs on perceived input. Draining the jitter
            // puts the easer to sleep within ~2 frames, so the direct write sticks and the view
            // stays on the target (no per-frame fight, no flicker). The next real user input
            // wakes the easer and glides the map back inside the engine's bounds, as normal.
            if (goblin::overlay_api::locate_target_clamped())
            {
                if (ctx.nav_frames->load(std::memory_order_relaxed) > 2)
                    ctx.nav_frames->store(2, std::memory_order_relaxed);
            }
            else if (ctx.nav_frames->load(std::memory_order_relaxed) < s_hold_frames)
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

    // Mob NAMES on the game's own non-boss enemy HP bar. The engine already draws the bar and
    // stored each on-screen bar position; we read (screenPos, name) POD entries from the host and
    // label them. Screen coords are the game's virtual 1920x1080 space -> scale to the backbuffer.
    // See docs/re/linux_enemy_healthbar_name_re_findings.md.
    static void draw_enemy_bar_names(ImGuiIO &io)
    {
        if (!*goblin::overlay_api::cfg_enemyNames_ptr())
            return;
        // Don't draw the in-world HUD over the world map / a menu (we render post-present, always on
        // top). The game hides the enemy bars there anyway; this stops our text punching through.
        if (goblin::overlay_api::world_map_open() || goblin::overlay_api::input_menu_open())
            return;
        goblin::EnemyBarLabel labels[8];
        int n = goblin::overlay_api::get_enemy_bar_labels(labels, 8);
        if (n <= 0)
            return;
        const float vscale = io.DisplaySize.x / 1920.0f; // game 1920x1080 space -> backbuffer
        const float tscale = *goblin::overlay_api::cfg_enemyNameScale_ptr();
        const float offY = *goblin::overlay_api::cfg_enemyNameOffsetY_ptr() * vscale;
        ImFont *font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * (tscale > 0.f ? tscale : 1.f);
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        for (int i = 0; i < n; ++i)
        {
            if (labels[i].name[0] == '\0')
                continue;
            const float sx = labels[i].sx * vscale;
            const float sy = labels[i].sy * vscale;
            const char *txt = labels[i].name;
            ImVec2 ts = ImGui::CalcTextSize(txt);
            ts.x *= tscale; ts.y *= tscale;
            // Centered on the bar's x, `offY` above the bar top. Shadow first for legibility.
            const ImVec2 p(sx - ts.x * 0.5f, sy - ts.y - offY);
            fg->AddText(font, fontSize, ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 205), txt);
            fg->AddText(font, fontSize, p, IM_COL32(255, 255, 255, 235), txt);
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
        draw_enemy_bar_names(io); // mob names on the game's enemy HP bar (independent of the minimap)
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

    // dx-bugs-backlog item 2 followup (the "cheap follow-on" UI half): the panel's close-hint
    // names the binding for the ACTIVE input device — the configurable gamepad combo (PR C)
    // while the pad drives, the F1 key otherwise. Same debounced flag hk_present's
    // cursor-recenter compensation uses.
    static std::string close_hint()
    {
        if (goblin::input::last_input_was_gamepad())
            return goblin::overlay_api::mask_to_combo_string(
                *goblin::overlay_api::cfg_overlayToggleGamepad_ptr());
        return "F1";
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
            ImGui::TextUnformatted(tr("Map for Goblins - ERROR"));
            ImGui::Separator();
            ImGui::TextWrapped("%s", tr("The mod's map folder (map\\MapStudio\\*.msb.dcx) could not be "
                                        "found. The mod cannot load its markers."));
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
                ImGui::TextUnformatted(tr("Map for Goblins - WARNING"));
                ImGui::Separator();
                ImGui::TextWrapped("%s", tr("The RE signatures were not resolved correctly. The mod "
                                            "will probably not work correctly (markers, graces or "
                                            "loot missing or wrong)."));
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
            ImGui::TextDisabled("%s", close_hint().c_str());
            ImGui::End();
        }
        else
        {
            // Full panel — live controls (post intents to the watcher thread).
            // Auto-fit to content (so it stops being clipped too small), clamped
            // to a sane min/max so it neither shrinks to nothing nor overflows the
            // screen; if content exceeds the max it scrolls.
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            // Max width 720 -> 840: translated labels (i18n) run longer than the English
            // source — French checkbox rows clipped at the old cap. Translations should
            // still aim for <= English+20% so the panel doesn't eat the screen.
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(360.0f, 240.0f),
                ImVec2(840.0f, io.DisplaySize.y * 0.92f));
            // Tabbed layout: a FIXED default size (user-resizable) instead of AlwaysAutoResize —
            // otherwise the window would jump size on every tab switch (each tab has different
            // content height). Per-tab content taller than the window scrolls inside it.
            ImGui::SetNextWindowSize(ImVec2(430.0f, 620.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Map for Goblins##large", nullptr, ImGuiWindowFlags_None);
            // Keep the per-category census warm: the watcher only runs the flag
            // sweep while the panel is on-screen (stamped here each frame).
            goblin::overlay_api::note_menu_visible();
            if (ImGui::Button(tr("[-] collapse")))
                g_large = false;
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%s close | %.0f fps"), close_hint().c_str(), io.Framerate);
            ImGui::Separator();

            // Settings search: type a keyword and only the matching parts of the panel stay
            // visible (matching sections auto-expand); clear the box to restore everything.
            // Matches section TITLES and the SETTING LABELS inside them (each block below
            // declares its keywords), so "opacity" finds Minimap and "altitude" finds the
            // flat checkbox. Separate from the "Sections & categories" search below, which
            // filters marker CATEGORIES, not panel settings.
            static char settings_q[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##settingsfilter", tr("find setting... (e.g. altitude, minimap zoom, spoiler)"),
                                     settings_q, sizeof(settings_q));
            panel::draw_gamepad_keyboard_button("##settingsfilter_kbd", settings_q, sizeof(settings_q));
            // Keywords per block = its title + the labels of the settings it contains; every
            // query token must hit (word-order-independent, accent/case-insensitive). Hits are
            // counted so an all-miss query can say so (last frame's count -- the blocks below
            // haven't evaluated yet this frame; 1-frame lag is invisible).
            panel::Filter f;
            f.q = settings_q;
            f.filtering = settings_q[0] != '\0';
            static int s_settings_hits = 0;
            if (f.filtering && s_settings_hits == 0)
                ImGui::TextDisabled(tr("no setting matches \"%s\" — clear the box to show all"), settings_q);
            ImGui::Separator();

            // In-game pause (dx-bugs-backlog item 4): flips the frame-step branch, world sim
            // freezes, this panel stays fully usable. Hidden when the signature didn't resolve.
            // Drawn in the Display tab (below) or, while the search filter is active, in the flat list.
            auto draw_pause = [&]() {
                if (goblin::pause::available() && f.match("pause game freeze stop world"))
                {
                    bool paused = goblin::pause::paused();
                    if (ImGui::Checkbox(tr("Pause the game (world sim freezes; menu stays usable)"), &paused))
                        goblin::pause::set_paused(paused);
                }
                // Auto-pause driven by opening/closing THIS panel (keyboard F1 or the gamepad combo) —
                // applied on the open/close edge in goblin_overlay.cpp. Config-backed, persists on Save.
                if (goblin::pause::available() && f.match("pause on open auto f1 gamepad freeze menu panel"))
                {
                    bool *p = goblin::overlay_api::cfg_pauseOnOpen_ptr();
                    ImGui::Checkbox(tr("Pause automatically while this panel is open (F1 / gamepad)"), p);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Freezes the world sim the moment you open F1 (or the gamepad\n"
                                                   "combo) and resumes on close. Only releases a pause it set\n"
                                                   "itself. Saved with your settings."));
                }
            };

            // The panel is organized into TABS by usage (Markers / Search / Quests / Display / Dev).
            // BUT the settings-search box is a cross-cutting filter: when it's active we hide the tabs
            // and draw every matching block in ONE flat list (a match may live in any tab), so a search
            // never hides its own results behind an unselected tab. Empty box → the tabs come back.
            if (f.filtering)
            {
                draw_pause();
                panel::draw_dev_icon_sections(ctx, f);   // dev/RE icon sections
                panel::draw_general_settings(ctx, f);    // master+save, toggles, scale, minimap
                panel::draw_item_search(ctx, f);         // find item / object
                panel::draw_sections_categories(ctx, f); // categories grid + ERR
                panel::draw_quest_browser(f);            // quest browser
                panel::draw_clustering(f);               // clustering
                panel::draw_dev_tools_danger(f);         // debug / dev tools / danger
                panel::draw_world_editor(f);             // world editor
            }
            else if (ImGui::BeginTabBar("##f1tabs", ImGuiTabBarFlags_None))
            {
                // Markers — the everyday "what shows on the map" controls.
                if (ImGui::BeginTabItem(tr("Markers")))
                {
                    panel::draw_sections_categories(ctx, f);  // categories grid (with icons) + ERR
                    panel::draw_clustering(f);                // clustering presets
                    ImGui::EndTabItem();
                }
                // Search — find an item/object and locate it on the map.
                if (ImGui::BeginTabItem(tr("Search")))
                {
                    panel::draw_item_search(ctx, f);
                    ImGui::EndTabItem();
                }
                // Quests — quest navigation / browser.
                if (ImGui::BeginTabItem(tr("Quests")))
                {
                    panel::draw_quest_browser(f);
                    ImGui::EndTabItem();
                }
                // Display — global rendering settings (scale, minimap, toggles) + pause.
                if (ImGui::BeginTabItem(tr("Display")))
                {
                    draw_pause();
                    panel::draw_general_settings(ctx, f);
                    ImGui::EndTabItem();
                }
                // Dev — RE/debug tooling: icon census, dev tools + danger zone, live world editor.
                if (ImGui::BeginTabItem(tr("Dev")))
                {
                    panel::draw_dev_icon_sections(ctx, f);
                    panel::draw_dev_tools_danger(f);
                    panel::draw_world_editor(f);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            s_settings_hits = f.hits;
            ImGui::End();
        }
    }

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
// Host→render direction: stable-name extern "C" exports so the host can resolve these via
// GetProcAddress (not ordinary dllimport — Slice D reloads this module, so the host must be able
// to re-resolve these by name after a fresh LoadLibrary, which a load-time-bound import can't do).
// Both DLLs statically link their own copy of imgui (separate global state per DLL), so before the
// real draw call each trampoline (a) points this module's imgui at the HOST's allocator triple —
// once per module load; the static resets on every hot reload — because imgui allocates via malloc
// wrappers (per-DLL CRT heap under /MT, NOT covered by goblin_render_new_override.cpp's operator-
// new routing) and render-side draws grow buffers the host later frees; and (b) sets the host's
// ImGuiContext current — see OverlayFrameCtx::imgui_ctx/imgui_alloc_fn.
namespace
{
    void apply_imgui_bindings(const goblin::overlay::OverlayFrameCtx *ctx)
    {
        static bool s_alloc_applied = false;
        if (!s_alloc_applied && ctx->imgui_alloc_fn && ctx->imgui_free_fn)
        {
            ImGui::SetAllocatorFunctions(ctx->imgui_alloc_fn, ctx->imgui_free_fn, ctx->imgui_alloc_ud);
            s_alloc_applied = true;
        }
        ImGui::SetCurrentContext(ctx->imgui_ctx);
    }
}
extern "C"
{
    __declspec(dllexport) void MFG_DrawPanel(const goblin::overlay::OverlayFrameCtx *ctx)
    {
        apply_imgui_bindings(ctx);
        goblin::overlay::draw_panel(*ctx);
    }
    __declspec(dllexport) void MFG_DrawWorldmapMarkers(bool menu_open, const goblin::overlay::OverlayFrameCtx *ctx)
    {
        apply_imgui_bindings(ctx);
        goblin::overlay::draw_worldmap_markers(menu_open, *ctx);
    }
    __declspec(dllexport) void MFG_DrawMinimapHud(const goblin::overlay::OverlayFrameCtx *ctx)
    {
        apply_imgui_bindings(ctx);
        goblin::overlay::draw_minimap_hud(*ctx);
    }
}
#endif
