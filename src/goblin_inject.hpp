#pragma once

#include <cstdint>
#include <vector>

namespace goblin
{
    void inject_map_entries();

    // Data pointers of MFG-injected WorldMapPointParam rows in the expanded
    // table. Populated by inject_map_entries(); consumed by
    // sanitize_injected_textids() after the FMG bank is built.
    const std::vector<uint8_t *> &injected_row_ptrs();

    // Rewrites rows baked with a primary completion flag to its alternative
    // once the alternative flag turns on (quest fights with two mutually-
    // exclusive outcome flags, e.g. the Sellen/Jerren academy battle).
    // Called periodically from the refresh loop.
    void apply_flag_or_pairs();

    // Toggle the WorldMapPointParam swap between vanilla and expanded states.
    // Used as an ERSC-hosting workaround: revert before host, re-apply after.
    void set_param_injection_active(bool active);
    bool is_param_injection_active();

    // Row ids used for both the TutorialParam rows AND the TutorialBody.fmg
    // entries holding each banner's STATIC text. Injected by
    // inject_tutorial_popup_rows() (param table) and goblin_messages
    // setup_messages() (FMG bank). All texts are static (no runtime FMG
    // rewrite — that approach crashed; each distinct message gets its own id).
    // Chosen just past the highest existing ERR codex id (9004250) — keeps
    // ids close to the original range to avoid any internal int32-cast or
    // size-bucket assumptions that hit far-out values.
    constexpr int TUTORIAL_FMG_ID_ON        = 9004251;  // "Map icons: ON"
    constexpr int TUTORIAL_FMG_ID_OFF       = 9004252;  // "Map icons: OFF"
    constexpr int TUTORIAL_FMG_ID_DUMP_OK   = 9004253;  // "Markers dumped"
    constexpr int TUTORIAL_FMG_ID_DUMP_FAIL = 9004254;  // "Marker dump failed - press again"

    // Inject the codex-toast TutorialParam rows for the F10/F9 banners. Rows
    // get menuType=0 (upper-left codex caption widget) with textId pointing
    // at TutorialBody.fmg entries injected by goblin_messages.
    // Returns true on success.
    bool inject_tutorial_popup_rows();

    // Fire an upper-left codex-style toast for one of the injected TutorialParam
    // rows (pass a TUTORIAL_FMG_ID_* id). Static text, no FMG rewrite — same
    // path as the F10 banner. Safe from any thread once init has run.
    void show_codex_toast(int tutorial_id);

    // Background thread polling the toggle hotkey.
    void toggle_hotkey_loop();

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the F10/gamepad personal show/hide and shows the toggle banner.
    void menu_auto_toggle_loop();
};
