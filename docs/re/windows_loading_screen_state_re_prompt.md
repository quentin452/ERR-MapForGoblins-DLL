# RE brief — loading-screen / world-load state (detect a STUCK load)

**Goal:** find the game state that says "a loading screen / warp / area transition is in progress" (and,
ideally, some progress signal), so MFG can add a **LOAD WATCHDOG**: if a load runs longer than N seconds
with no progress, log it + on-screen warn, and — where possible — surface WHY. A stuck loading screen with
no diagnosis is one of the most recurrent, opaque failures in ER modding (a bad warp target, a missing
map/asset, a mod conflict); a watchdog + a state dump turns "frozen, no idea" into an actionable log.
Static Ghidra on `D:\ghidra_proj2\ER`, App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only; the
DLL is in-process (Linux/Proton) and already ships a present-thread FREEZE watchdog (`goblin_freeze_watchdog.cpp`)
we can mirror.

## Motivating bug
Warping (`LuaWarp_01` / `goblin::warp::to_grace`) to an **undiscovered** grace produced an **infinite
loading screen** (now gated out in MFG, but the general class — a load that never finishes — needs
detection). NB the freeze watchdog does NOT catch this: during a load the present thread often keeps
beating (the loading screen renders), so it's a LOAD stall, not a present stall — a distinct signal.

## What to find (priority order)
1. **A "load in progress" flag/state.** The manager + offset that flips true when a fade-to-load / area
   transition / warp begins and false when the world is playable again. Candidates: `CS::FadeSystem` /
   `CS::FadePlate`, the black-screen fade state, the `CS::CSSessionManager` / world-load state machine, the
   Scaleform loading-screen menu (a `CSMenu` that's present only while loading), or a WorldChrMan /
   `CS::CSFD4LocationStepController` "world ready" bit. Give: how to resolve it live (singleton slot / menu
   walk) + the exact bool/enum offset + its values (loading vs ready).
2. **A progress or phase signal (if any).** A load percentage, a phase enum (fade-out → stream → fade-in),
   or a step counter that ADVANCES during a healthy load — so the watchdog can distinguish "slow but
   progressing" from "genuinely stuck" (no change for N s). Even a coarse phase enum helps.
3. **The warp/transition target (for the WHY).** When a warp/load starts, is the destination (mapId /
   bonfire id / spawn point) readable somewhere? Logging "stuck loading → target mapId m60_xx / grace
   <id>" is the actionable part. The player MapId slot is already RE'd (`PLAYER_MAPID_SLOT`); does it update
   at load start (target) or only after (arrived)?
4. **(optional) A load-failure / assert path.** Does a bad target hit an error/assert routine we could hook
   to catch the failure directly rather than by timeout?

## Deliverable
Enough to add a load watchdog (mirror `goblin_freeze_watchdog.cpp`): poll the load-in-progress flag on a
background thread; when it's been true > threshold (ini, e.g. 30 s) with no progress-signal change, write
`logs/MapForGoblins_load_stall_<pid>.txt` with { elapsed, phase, target mapId/grace, thread stacks } +
a codex toast. Report: the flag manager+offset+values (item 1), the progress/phase signal (item 2), and
the target source (item 3). Findings → `docs/re/windows_loading_screen_state_re_findings.md`. This is a
general modding-diagnostics win, not MFG-specific.
