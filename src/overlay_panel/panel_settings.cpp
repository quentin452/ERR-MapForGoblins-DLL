// F1 panel — general settings: master toggle + Save, the flat option checkboxes, the
// gamepad combo recorder, marker scale, and the minimap block. Moved verbatim from
// goblin_overlay_render.cpp::draw_panel in the split (big-files refactor item 1).

#include "panel_internal.hpp"
#include "worldmap/map_renderer.hpp"  // ui_rect_* (UI exclusion-zone editor)
#include "goblin_i18n.hpp"

#include <cstdio>
#include <string>
#include "goblin_overlay_render_api.hpp"
#include "goblin_config.hpp"  // overlayLanguage (live language combo)

namespace goblin::overlay::panel
{
using goblin::i18n::tr;  // overlay UI localization (lang/<code>.txt)

// Force the in-game minimap to stay drawn while its settings section is expanded, even with the vmap /
// native map open — otherwise tuning the minimap sliders gives no live feedback (map_renderer.cpp:2541
// hard-hides the minimap under any full map). Stamped every frame the section is open; expires ~1 frame
// after the settings stop drawing (F1 closed / section collapsed). map_renderer.cpp reads the accessor.
namespace { int s_minimap_focus_frame = -1000; }
bool minimap_settings_focused() { return ImGui::GetFrameCount() - s_minimap_focus_frame <= 1; }

namespace
{
// "#RRGGBB" -> rgb[3] in 0..1 (silently leaves rgb untouched on a malformed value).
void hex_to_rgb(const std::string &hex, float rgb[3])
{
    if (hex.size() != 7 || hex[0] != '#') return;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 3; ++i)
    {
        int hi = nib(hex[1 + i * 2]), lo = nib(hex[2 + i * 2]);
        if (hi < 0 || lo < 0) return;
        rgb[i] = (hi * 16 + lo) / 255.0f;
    }
}

// rgb[3] in 0..1 -> "#RRGGBB".
std::string rgb_to_hex(const float rgb[3])
{
    auto clamp255 = [](float v) { int n = (int)(v * 255.0f + 0.5f); return n < 0 ? 0 : (n > 255 ? 255 : n); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", clamp255(rgb[0]), clamp255(rgb[1]), clamp255(rgb[2]));
    return buf;
}

// One #RRGGBB config-string bound to an ImGui ColorEdit3 (writes back on edit).
void color_edit_hex(const char *label, std::string &hex_cfg)
{
    float rgb[3] = {1, 1, 1};
    hex_to_rgb(hex_cfg, rgb);
    if (ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs))
        hex_cfg = rgb_to_hex(rgb);
}
} // namespace

void draw_general_settings(const OverlayFrameCtx &ctx, Filter &f)
{
    // Master on/off + Save.
    bool icons_on = goblin::overlay_api::icons_enabled();
    if (ImGui::Checkbox(tr("Show icons (master)"), &icons_on))
        goblin::overlay_api::set_icons_enabled(icons_on);
    ImGui::SameLine();
    static double saved_at = -10.0;
    if (ImGui::Button(tr("Save to INI")))
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
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", tr("Saved to INI"));
    }

    // Overlay UI language — LIVE switch (same-frame table swap; everything below this
    // line already draws translated). tr() + set_language() both run on this present
    // thread, so no locking (see goblin_i18n.hpp). Persist via "Save to INI" as usual.
    if (f.match("language langue overlay ui english french francais traduction translation"))
    {
        ImGui::Text("%s", tr("Language:"));
        ImGui::SameLine();
        std::string &cfg_lang = goblin::config::overlayLanguage;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("##uilang", cfg_lang.c_str()))
        {
            auto pick = [&](const std::string &code) {
                if (ImGui::Selectable(code.c_str(), cfg_lang == code))
                {
                    cfg_lang = code;
                    goblin::i18n::set_language(code.c_str());
                }
            };
            pick("auto");
            pick("en");
            // One entry per lang/<code>.txt on disk (scanned only while the combo is open).
            for (const std::string &c : goblin::i18n::available_languages())
                if (c != "en")
                    pick(c);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Overlay UI language (lang/<code>.txt next to the DLL).\n"
                                       "Switches live; Save to INI to persist. Game content\n"
                                       "names stay in the game's own language."));
    }

    // Map-fragment gate (live; persists via "Save to INI"). When on, a marker stays hidden
    // until the player has acquired that area's map-fragment ITEM (fragment event flag).
    if (f.match("require map fragments hide area icons until fragment found"))
        ImGui::Checkbox(tr("Require map fragments (hide an area's icons until its fragment is found)"),
                        goblin::overlay_api::cfg_requireMapFragments_ptr());

    // DIAG: draw ONLY the no-bake residual (Baked-source markers; disk/live twins already
    // deduped away). Fly the world + eyeball each spot — real loot the live pass misses
    // (coverage gap) vs a phantom the bake invented (bake bug). See nobake_scoreboard.md.
    // Dev-gated behind Verbose logging (same pattern as "Locate debug (dev)") — a bake/live
    // triage tool, not a user preference, so it stays out of the default settings list.
    if ((*goblin::overlay_api::cfg_debugLogging_ptr()) && f.match("baked only diag no-bake residual"))
    {
        ImGui::Checkbox("Baked-only (diag: show just the no-bake residual)",
                        goblin::overlay_api::cfg_bakedOnly_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hides every marker the live disk/memory passes already cover,\n"
                              "leaving only the markers still coming from the static bake.\n"
                              "Use it to judge each leftover: real loot we fail to source live\n"
                              "(coverage miss) vs a stale/invented spot (bake bug).");
    }

    // Collected/cleared graying (overlay map; live, persists via "Save to INI").
    // On = dim looted items / killed bosses (cleared bosses get a checkmark);
    // hide_collected switches dim → remove (legacy native-map behaviour).
    if (f.match("gray grey collected cleared markers dim looted killed bosses hide instead"))
    {
        ImGui::Checkbox(tr("Gray collected/cleared markers (dim looted items & killed bosses)"),
                        goblin::overlay_api::cfg_collectedGraying_ptr());
        if ((*goblin::overlay_api::cfg_collectedGraying_ptr()))
        {
            ImGui::SameLine();
            ImGui::Checkbox(tr("hide instead"), goblin::overlay_api::cfg_hideCollected_ptr());
        }
    }

    // Merge co-located identical-item loot markers into one "xN". Pure render decision (the
    // grouping is annotated once at build) → instant toggle, no bucket rebuild.
    if (f.match("stack identical items merge same-item nodes"))
        ImGui::Checkbox(tr("Stack identical items (merge same-item nodes within ~5m)"),
                        goblin::overlay_api::cfg_stackIdenticalItems_ptr());

    // Major-region name labels (overlay map; live, persists via "Save to INI").
    if (f.match("show region labels major-region names map"))
        ImGui::Checkbox(tr("Show region labels (major-region names on the map)"),
                        goblin::overlay_api::cfg_showRegionLabels_ptr());

    // Redify boss markers (overlay port of the legacy red-skull iconId; live,
    // persists via "Save to INI"). Tints WorldBosses markers red (overworld +
    // dungeon bosses); collected/cleared graying still takes precedence.
    if (f.match("red boss markers tint icons skull"))
        ImGui::Checkbox(tr("Red boss markers (tint boss icons red)"),
                        goblin::overlay_api::cfg_redifyBossIcons_ptr());

    // DX item 7: up/down altitude badge for markers above/below the player (player's map only).
    if (f.match("altitude arrows up above down below player badge"))
        ImGui::Checkbox(tr("Altitude arrows (up = above / down = below player)"),
                        goblin::overlay_api::cfg_altitudeCue_ptr());

    // Grace rendering: overlay draws all graces (discovered=colour, undiscovered=grey).
    if (f.match("overlay graces draw all gpu sprite engine cpu baked atlas"))
        ImGui::Checkbox(tr("Overlay graces (draw all graces ourselves)"),
                        goblin::overlay_api::cfg_graceOverlay_ptr());

    // Native landmark-pin suppression (areaNo flips applied by the watcher; effect on the
    // NEXT map open, so the nudge just re-decides — no live rebuild needed here).
    if (f.match("hide native landmark pins duplicate suppress erdtree dungeon church"))
    {
        if (ImGui::Checkbox(tr("Hide native landmark pins (when our category re-draws them)"),
                            goblin::overlay_api::cfg_landmarkSuppressNative_ptr()))
            goblin::overlay_api::request_native_landmark_reapply();
        ImGui::SameLine();
        ImGui::TextDisabled("↻");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Per category: only World-landmark categories that are toggled ON get their\n"
                                       "native pin hidden (no duplicate). Categories OFF keep the game's own pin.\n"
                                       "Changes show the NEXT time the map is opened (the game rebuilds its pin\n"
                                       "list then). The affected category rows carry the same \xE2\x86\xBB mark."));
    }

    // Spoiler-free loot (overlay port of anonymous_loot; live, persists via
    // "Save to INI"). Lot-backed loot markers draw as a gray "?" with a generic
    // label instead of the real item icon/name.
    if (f.match("spoiler-free loot gray anonymous randomizer hide item"))
    {
        ImGui::Checkbox(tr("Spoiler-free loot (gray \"?\" instead of the item)"),
                        goblin::overlay_api::cfg_anonymousLoot_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Every loot marker shows a gray \"?\" and only its location,\n"
                                       "hiding the real item (useful with randomizers). Markers still\n"
                                       "gray out when collected; category show/hide is unaffected."));
    }

    // Gamepad overlay-toggle combo (dx-bugs-backlog PR C item 3). Recorder arms the
    // XInput poll in hk_present; first nonzero button read there wins and saves.
    if (f.match("gamepad toggle combo record controller buttons"))
    {
        ImGui::Text(tr("Gamepad toggle combo: %s"),
                    goblin::overlay_api::mask_to_combo_string((*goblin::overlay_api::cfg_overlayToggleGamepad_ptr())).c_str());
        ImGui::SameLine();
        if (*ctx.gamepad_combo_recording)
        {
            // Two phases: first wait for the button that ARMED recording (e.g. gamepad-nav A
            // on this very widget) to fully release, THEN start listening — otherwise that
            // same activating press gets captured as the whole combo.
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                "%s", *ctx.gamepad_combo_ready ? tr("Press buttons now…") : tr("Release all buttons…"));
        }
        else if (ImGui::SmallButton(tr("Record gamepad combo")))
        {
            *ctx.gamepad_combo_recording = true;
            ctx.gamepad_combo_reject_reason->clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Click, then press the button combo on your controller (default Y+R3).\n"
                                       "The first combo read is captured and saved to the ini immediately."));
        if (!ctx.gamepad_combo_reject_reason->empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", ctx.gamepad_combo_reject_reason->c_str());
    }

    // On-screen keyboard layout (dx-bugs-backlog PR C-2 part 2). Like every other plain
    // setting here, this only persists via "Save to INI" below — no immediate auto-save.
    if (f.match("gamepad keyboard layout alphabetical qwerty virtual on-screen"))
    {
        ImGui::Text("%s", tr("Gamepad keyboard layout:"));
        ImGui::SameLine();
        int kbd_layout = (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr());
        if (ImGui::RadioButton(tr("Alphabetical"), &kbd_layout, 0))
            (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) = static_cast<uint8_t>(kbd_layout);
        ImGui::SameLine();
        if (ImGui::RadioButton("QWERTY", &kbd_layout, 1))
            (*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) = static_cast<uint8_t>(kbd_layout);
    }

    // Overlay marker scale (live preview; persists via "Save to INI"). Final
    // size = resolution-relative base × master × per-type scale.
    const bool show_scale = f.match("marker scale overlay map master category icons grace "
                                    "offset cluster piles size motion delay frames zoom reset");
    if (f.filtering && show_scale) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_scale && ImGui::CollapsingHeader(tr("Marker scale (overlay map)")))
    {
        scale_control(tr("Master"), goblin::overlay_api::cfg_overlayMasterScale_ptr(), 0.3f, 3.0f, 0.05f, 0.25f, "%.2f");
        scale_control(tr("Category icons"), goblin::overlay_api::cfg_overlayIconScale_ptr(), 0.3f, 10.0f, 0.05f, 0.25f, "%.2f");
        scale_control(tr("Marker motion delay (frames)"), goblin::overlay_api::cfg_viewDelayFrames_ptr(), 0.0f, 7.0f, 0.1f, 0.5f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Project markers this many present-frames behind the eased basemap.\n"
                                       "Pan the map: raise if markers LEAD (snap back on stop), lower if they TRAIL.\n"
                                       "1.0 = default. Tune to kill the pan/zoom re-adjust, then Save to INI."));
        ImGui::Checkbox(tr("Delay zoom too"), goblin::overlay_api::cfg_viewDelayZoom_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("ON (default): the motion delay applies to zoom as well as pan.\n"
                                       "If markers TELEPORT for one frame on each mouse-wheel zoom step,\n"
                                       "turn this OFF — markers then use the live zoom while still delaying pan."));
        if (ImGui::SmallButton(tr("Reset##scale")))
        {
            (*goblin::overlay_api::cfg_overlayMasterScale_ptr()) = 1.0f;
            (*goblin::overlay_api::cfg_overlayIconScale_ptr()) = 1.2f;     // match the schema defaults
            (*goblin::overlay_api::cfg_viewDelayFrames_ptr()) = 1.0f;
            (*goblin::overlay_api::cfg_viewDelayZoom_ptr()) = true;
        }
        ImGui::TextDisabled("%s", tr("Slider = coarse; type in the box or use its +/- arrows for an exact\n"
                                     "value (Ctrl+Click the slider also types). × a resolution-relative\n"
                                     "base. Save to INI to persist."));
    }

    // In-game minimap HUD (foundation; overworld-only, north-up). Live; persists.
    const bool show_minimap = f.match("minimap in-game hud show corner zoom radius opacity "
                                      "anchor right bottom offset north-up");
    if (f.filtering && show_minimap) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_minimap && ImGui::CollapsingHeader(tr("Minimap (in-game HUD)")))
    {
        s_minimap_focus_frame = ImGui::GetFrameCount();  // keep the minimap drawn over the vmap while tuning
        ImGui::Checkbox(tr("Show minimap (corner HUD during gameplay)"),
                        goblin::overlay_api::cfg_showMinimap_ptr());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("A small north-up minimap in the screen corner showing nearby\n"
                                       "markers around you during play. OVERWORLD only for now\n"
                                       "(underground player position isn't reliable yet)."));
        // Max raised 0.30 -> 0.60 (user feedback 2026-07-01: 0.30 was still too
        // zoomed-out/small at max). Default also raised, see minimapZoom's declaration.
        // AlwaysClamp: ImGui's Ctrl+Click-to-type on a slider does NOT clamp to
        // [min,max] by default -- a typed value beyond what's shown could be saved to
        // the INI, then silently reset on the next load by the (now correct, but still
        // real) per-field range clamp in goblin_config.cpp. Keep what's shown and what's
        // stored always in sync.
        ImGui::SliderFloat(tr("Zoom (px/world)"), goblin::overlay_api::cfg_minimapZoom_ptr(), 0.02f, 5.0f, "%.3f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat(tr("Radius (px)"), goblin::overlay_api::cfg_minimapSize_ptr(), 60.0f, 300.0f, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat(tr("Opacity"), goblin::overlay_api::cfg_minimapOpacity_ptr(), 0.0f, 1.0f, "%.2f");
        ImGui::Checkbox(tr("Anchor right"), goblin::overlay_api::cfg_minimapAnchorRight_ptr());
        ImGui::SameLine();
        ImGui::Checkbox(tr("Anchor bottom"), goblin::overlay_api::cfg_minimapAnchorBottom_ptr());
        ImGui::SliderFloat(tr("Offset X"), goblin::overlay_api::cfg_minimapOffsetX_ptr(), 0.0f, 600.0f, "%.0f");
        ImGui::SliderFloat(tr("Offset Y"), goblin::overlay_api::cfg_minimapOffsetY_ptr(), 0.0f, 600.0f, "%.0f");
        ImGui::TextDisabled("%s", tr("North-up. Hidden while the world map is open. Save to INI to persist."));
    }

    // Enemy names via the engine's own native tag (no ImGui overlay). Live; persists.
    const bool show_enemy = f.match("enemy bars mob name health bar hp label native tag npcname");
    if (f.filtering && show_enemy) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_enemy && ImGui::CollapsingHeader(tr("Enemy bars (mob names)")))
    {
        bool *master = goblin::overlay_api::cfg_enemyNames_ptr();
        ImGui::Checkbox(tr("Name non-boss enemies"), master);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Name the game's own non-boss enemy tags. The name is resolved\n"
                                       "live from the active install and fed to the engine's OWN red\n"
                                       "tag (NpcParam.nameId -> NpcName), so the game renders it itself\n"
                                       "— correct font/accents, frame-synced. Bosses are already named.\n"
                                       "A mob with no name in the game data stays unnamed. Takes effect\n"
                                       "on the enemy's next name refresh; turning it OFF stops naming\n"
                                       "NEW enemies (already-named ones clear on reload)."));

        ImGui::BeginDisabled(!*master);
        // Which categories get named. Category = teamType (hostile) then resolver tier (field-boss / mob).
        ImGui::Checkbox(tr("Regular mobs"), &goblin::config::nameEnemyMobs);
        ImGui::Checkbox(tr("Field bosses / minibosses"), &goblin::config::nameEnemyBosses);
        ImGui::Checkbox(tr("Hostile NPCs / invaders"), &goblin::config::nameEnemyHostiles);

        ImGui::Separator();
        ImGui::Checkbox(tr("Colorize names by category"), &goblin::config::enemyNameColorize);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("EXPERIMENTAL. Tints each name via an HTML color tag fed into the\n"
                                       "game's own text field. Works only if that field parses inline\n"
                                       "HTML — if it doesn't, names show the raw <font> tags instead.\n"
                                       "Verify in-world before relying on it. Applies to newly-named\n"
                                       "enemies (already-named types recolor on reload)."));
        ImGui::BeginDisabled(!goblin::config::enemyNameColorize);
        color_edit_hex(tr("Mob color"), goblin::config::enemyNameColorMob);
        color_edit_hex(tr("Field-boss color"), goblin::config::enemyNameColorBoss);
        color_edit_hex(tr("Hostile color"), goblin::config::enemyNameColorHostile);
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::TextDisabled("%s", tr("Live. Save to INI to persist."));
    }

    // User-drawn "no overlay icons here" zones — self-service fix for icons drawing over
    // the game's own map UI (we render post-present, always on top). Stored in 1920x1080
    // virtual-canvas units so the zones hold at every resolution.
    const bool show_uiex = f.match("ui exclusion zones clipping clip rectangles hide icons "
                                   "under game menu dial clock zone");
    if (f.filtering && show_uiex) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (show_uiex && ImGui::CollapsingHeader(tr("UI exclusion zones (map clipping)")))
    {
        ImGui::Checkbox(tr("Enable clipping (hide icons under game UI)"),
                        goblin::overlay_api::cfg_clipGameUi_ptr());
        bool edit = goblin::worldmap::ui_rect_edit();
        if (ImGui::Checkbox(tr("Edit zones (open the world map)"), &edit))
            goblin::worldmap::set_ui_rect_edit(edit);
        if (edit)
            ImGui::TextDisabled("%s", tr("On the MAP: drag = new zone, right-click a zone = delete."));
        const int n = goblin::worldmap::ui_rect_count();
        for (int i = 0; i < n; ++i)
        {
            float r[4];
            if (!goblin::worldmap::ui_rect_get(i, r)) break;
            ImGui::Text(tr("Zone %d: (%.0f,%.0f)-(%.0f,%.0f)"), i + 1, r[0], r[1], r[2], r[3]);
            ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::SmallButton(tr("Delete")))
                goblin::worldmap::ui_rect_delete(i);
            ImGui::PopID();
        }
        if (n > 0 && ImGui::SmallButton(tr("Clear all zones")))
            goblin::worldmap::ui_rect_clear();
        ImGui::TextDisabled("%s", tr("Zones are saved in 1920x1080 virtual units - they work at every\n"
                                     "resolution. Save to INI to persist."));

        // ERR day/night dial — a round region the rectangle zones can't express. Placement
        // handles on the map + precise sliders; the values are the ini-backed cfg::dial* globals.
        if (goblin::overlay_api::err_features())
        {
            ImGui::Separator();
            ImGui::TextUnformatted(tr("ERR day/night dial exclusion"));
            bool dedit = goblin::worldmap::dial_edit();
            if (ImGui::Checkbox(tr("Edit dial (open the world map)"), &dedit))
                goblin::worldmap::set_dial_edit(dedit);
            if (dedit)
                ImGui::TextDisabled("%s", tr("On the MAP: cyan dot = move disc, yellow = radius, magenta = move pill."));
            ImGui::SliderFloat(tr("Disc X"), goblin::overlay_api::cfg_dialDiscX_ptr(), 0.0f, 1920.0f, "%.0f");
            ImGui::SliderFloat(tr("Disc Y"), goblin::overlay_api::cfg_dialDiscY_ptr(), 0.0f, 1080.0f, "%.0f");
            ImGui::SliderFloat(tr("Disc radius"), goblin::overlay_api::cfg_dialDiscR_ptr(), 0.0f, 500.0f, "%.0f");
            ImGui::SliderFloat(tr("Pill left"), goblin::overlay_api::cfg_dialPillX0_ptr(), 0.0f, 1920.0f, "%.0f");
            ImGui::SliderFloat(tr("Pill top"), goblin::overlay_api::cfg_dialPillY0_ptr(), 0.0f, 1080.0f, "%.0f");
            ImGui::SliderFloat(tr("Pill right"), goblin::overlay_api::cfg_dialPillX1_ptr(), 0.0f, 1920.0f, "%.0f");
            ImGui::SliderFloat(tr("Pill bottom"), goblin::overlay_api::cfg_dialPillY1_ptr(), 0.0f, 1080.0f, "%.0f");
            ImGui::SliderFloat(tr("Fade margin"), goblin::overlay_api::cfg_dialFadeMargin_ptr(), 0.0f, 200.0f, "%.0f");
            ImGui::TextDisabled("%s", tr("Disc radius 0 = disc off; pill bottom <= top = pill off.\n"
                                         "Fade margin = soft dim band around the dial (0 = hard edge). Save to INI to persist."));
        }
    }
}
} // namespace goblin::overlay::panel
