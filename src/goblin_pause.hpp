#pragma once

// In-game pause. Backed by the character-update freeze (SetDisableAllChrUpdate, below) — ER's own
// cutscene freeze — NOT the old frame-step branch flip (removed 2026-07-06: its resume hitch grew with
// pause duration). Freezes every ChrIns in pose; render/UI/input stay live; resume is INSTANT even after
// minutes. The pause API (available/paused/set_paused) is the all-chr freeze; the F1 panel, pauseOnOpen,
// and the fullscreen vmap all drive it.
//
// Exposed to the render module (F1 "Pause game" checkbox) and to the debug RPC (`pause 0|1|toggle` and
// `freeze 1|0|toggle|enemies`, `paused=` in status — lets a driver clear an unfocused-window pause).

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)

namespace goblin::pause
{
    // False when the signature didn't resolve (game update drift) — callers hide the UI /
    // return an error instead of poking a wrong address. First call does the scan (lazy, once).
    GOBLIN_RENDER_API bool available();

    GOBLIN_RENDER_API bool paused();
    GOBLIN_RENDER_API void set_paused(bool paused);  // = request_freeze(FREEZE_MANUAL, paused)

    // Freeze reasons — OR-combined: the world stays frozen while ANY reason is active, so the fullscreen
    // vmap freeze and the F1-panel auto-pause don't stomp each other (closing one doesn't unfreeze while the
    // other is still open). Each caller owns its own bit; freeze = (mask != 0).
    enum FreezeReason : unsigned { FREEZE_MANUAL = 1u, FREEZE_PANEL = 2u, FREEZE_VMAP = 4u };
    GOBLIN_RENDER_API void request_freeze(unsigned reason, bool on);

    // ── Character-update freeze (ER's own cutscene freeze — preferred over the branch-flip pause) ──
    // Calls CS::CSEventUtility::SetDisableAllChrUpdate (FUN_1405f4d40): freezes every ChrIns (player +
    // enemies + NPCs) in pose while render / UI / input stay live. Resume is INSTANT even after minutes
    // frozen (a disabled chr isn't updated → no dt accumulates → nothing to catch up), unlike set_paused's
    // branch flip (resume hitch ∝ duration). Used to freeze the world while the fullscreen vmap stands in
    // for the native map → the map is always openable and no combat can start (no combat detection needed).
    // enemies_only exempts the player via the [ChrIns+0x531] XOR-disable bit (UNTESTED theory; safe no-op if
    // wrong). game_timestep_freeze_re_findings.md "SOLVED".
    GOBLIN_RENDER_API bool chr_freeze_available();  // false if the signature didn't resolve (update drift)
    GOBLIN_RENDER_API bool chr_frozen();
    GOBLIN_RENDER_API void set_chr_freeze(bool freeze, bool enemies_only = false);
}
