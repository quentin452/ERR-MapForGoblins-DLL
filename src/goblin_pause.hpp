#pragma once

// In-game pause (dx-bugs-backlog item 4). Flips the frame-step `je` found by
// goblin::sig::PAUSE_BRANCH (technique from iArtorias/elden_pause) — pausing the world
// simulation without touching input or rendering, so the F1 panel (and the debug RPC) stay
// fully usable while paused. The branch byte IS the state, so paused() also reflects a pause
// set by any other patcher of the same branch (PauseTheGame.dll uses the identical flip; ship
// ONE of the two, not both — two togglers XOR each other's state).
//
// Exposed to the render module (F1 "Pause game" checkbox) and to the debug RPC
// (`pause 0|1|toggle`, `paused=` in status — lets a driver clear an unfocused-window pause
// before scripting the game).

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)

namespace goblin::pause
{
    // False when the signature didn't resolve (game update drift) — callers hide the UI /
    // return an error instead of poking a wrong address. First call does the scan (lazy, once).
    GOBLIN_RENDER_API bool available();

    GOBLIN_RENDER_API bool paused();
    GOBLIN_RENDER_API void set_paused(bool paused);
}
