// F1 panel — general settings: master toggle + Save, the flat option checkboxes, the
// gamepad combo recorder, marker scale, and the minimap block. Moved verbatim from
// goblin_overlay_render.cpp::draw_panel in the split (big-files refactor item 1).

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"

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

    // ("Baked-only" diag moved to the Dev tab — it's a bake/live triage tool, not a user preference.)

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

    // Virtual map on the game map key: open the fullscreen Virtual World Map when the
    // player opens the map, instead of the native map (live; persists via "Save to INI").
    if (f.match("virtual map on map key vmap native fullscreen open"))
        ImGui::Checkbox(tr("Virtual map on map key (open the Virtual World Map instead of the native map)"),
                        goblin::overlay_api::cfg_vmapOnMapKey_ptr());

    // Gamepad sensitivity for the vmap (reticle move + left-stick pan speed; live, persists via Save to INI).
    if (f.match("gamepad sensitivity vmap reticle stick pan speed controller"))
        ImGui::SliderFloat(tr("Gamepad sensitivity (vmap reticle + pan)"),
                           goblin::overlay_api::cfg_gamepadSensitivity_ptr(), 0.2f, 4.0f, "%.2f");

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

    // (grace_overlay + hide-native-landmark-pins toggles retired: baked ON — the overlay is the sole
    // grace/landmark source now the native map is retired. See f1_settings_imgui_only_classification.md.)

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
        // Level sub-option (only meaningful with spoiler-free on): light hides just the randomized
        // loot; aggressive is a full "blackout" for a blind randomizer run.
        if (*goblin::overlay_api::cfg_anonymousLoot_ptr())
        {
            bool *aggr = goblin::overlay_api::cfg_anonymousLootAggressive_ptr();
            ImGui::Indent();
            if (ImGui::RadioButton(tr("Light — randomized loot only"), !*aggr))
                *aggr = false;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Hide only markers whose identity is randomized:\n"
                                           "treasure/enemy drops + farmable drops. Bosses,\n"
                                           "pieces, kindling and landmarks keep their names."));
            if (ImGui::RadioButton(tr("Aggressive — blackout (for randomizers)"), *aggr))
                *aggr = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Full blackout: EVERY marker except graces shows \"?\"\n"
                                           "(bosses, pieces, kindling, POI/landmarks). Positions only.\n"
                                           "Bosses keep a bigger red \"?\" so a threat still reads."));
            ImGui::Unindent();
        }
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
        // (marker motion-delay sliders retired: native-basemap-only, baked to the tuned default now
        // the vmap draws its own basemap — see f1_settings_imgui_only_classification.md.)
        if (ImGui::SmallButton(tr("Reset##scale")))
        {
            (*goblin::overlay_api::cfg_overlayMasterScale_ptr()) = 1.0f;
            (*goblin::overlay_api::cfg_overlayIconScale_ptr()) = 1.2f;     // match the schema defaults
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
        ImGui::EndDisabled();

        // (No name-color option: the game's native enemy tag is always red — it force-recolors the field
        // after we set the text, so an injected color can't take. Coloring would need a HUD gfx edit.)
        ImGui::TextDisabled("%s", tr("Live. Save to INI to persist."));
    }

    // (The "UI exclusion zones (map clipping)" section — user zone editor + ERR day/night dial
    // placement — was removed with the vmap-only collapse: it only clipped overlay markers under the
    // retired native map's own UI, moot now the vmap owns its whole surface.)
}
} // namespace goblin::overlay::panel
