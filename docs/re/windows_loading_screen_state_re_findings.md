# RE findings — loading-screen / world-load state (stuck-load watchdog)

Answers `docs/re/windows_loading_screen_state_re_prompt.md`. Static Ghidra on `D:\ghidra_proj2\ER`
(App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only). Scripts `D:\ghidra_scripts_mfg\mfg_load.java`.
Goal: signals for a LOAD watchdog (mirror `goblin_freeze_watchdog.cpp`) — the freeze watchdog misses a stuck
load because the present thread keeps beating (the loading screen renders).

---

## 0. TL;DR — a working watchdog today + a precise phase signal

Two complementary signals, both in ELDEN RING's world-load machinery:

1. **Anchor (pinned NOW, zero new RE): `LocalPlayer == null`.** `LocalPlayer = *(WorldChrMan + 0x1E508)`,
   `WorldChrMan = [er+0x3d65f88]` (both already pinned — `WCM_FINDER`, `windows_player_pos_RESOLVED`). It is
   **null while the world is not playable** (loading / area transition / title / menu-only). Gate out the
   title/menu case with the already-pinned `CSMENUMAN_SLOT`, and "LocalPlayer null in-session for > N s" is a
   stuck-load signal that MFG can read immediately.
2. **Precise (phase machine): `CSFD4LocationStep + 0x48` = the area-transition step/phase index.**
   `CSFD4LocationStep` (vtable `er+0x2b6b750`, ctor `FUN_140b40c00` er+0xb40c00, size `0xb8`) is the
   **world-load / area-transition STEP TASK**. Its vtable getters prove field `+0x48` is the current step:
   - `vtable[5] = FUN_140b413c0` → `return *(int*)(step + 0x48);`  (the phase index)
   - `vtable[4] = FUN_140b41460` → `return *(int*)(step + 0x48) == -1;`  (idle test)

   So **`step+0x48 == -1` ⇒ idle (no transition); `>= 0` ⇒ a load phase is in progress**, and the value
   **advances** through phases during a healthy load. That gives both "load in progress" AND the
   progress/phase signal to distinguish "slow but moving" from "genuinely stuck" (unchanged for N s).

**Watchdog design (mirror `goblin_freeze_watchdog.cpp`):** a background thread polls ~1 Hz; "loading" =
`LocalPlayer==null` (anchor) or `LocationStep+0x48 != -1` (precise); "stuck" = loading true > threshold (ini,
e.g. 30 s) with the phase field unchanged; on stuck → write `logs/MapForGoblins_load_stall_<pid>.txt` with
{ elapsed, phase (`+0x48`), target mapId (`PLAYER_MAPID_SLOT`), all-thread stacks } + a codex toast.

---

## 1. The world-load subsystem — class map

| Class | vtable (er+) | role | key field |
|-------|--------------|------|-----------|
| **`CS::CSFD4LocationStep`** | `0x2b6b750` | **area-transition/world-load step task** (a `CSStepTask`/`FD4StepTaskBase`) | **`+0x48` step/phase index** (`-1` = idle) |
| `CS::CSFD4FadeSystem` | `0x2b6a458` | screen fade compositor (fade-to-black on loads/cutscenes) | final RGB `+0x20/+0x24/+0x28`, **alpha `+0x2c`** (1.0 = opaque) |
| `CS::CSFD4FadePlate` | `0x2b6a170` | one fade plate (blended by FadeSystem) | — |
| `CS::CSFakeLoadingScreen` | (Imp `0x2b803b8`) | the **fast-travel / warp loading overlay** (the grace-warp bug's screen) | active while warping |
| `CS::CSSessionManager` | (Imp `0x2b9a0c8`) | session / world-session state machine | — |
| `CS::LoadingScreen` | `0x2afaeb0` | the loading-screen **`CSMenu`** (`CSMenuFrameComponent`/`Visible`) | present in CSMenuMan while loading |
| `CS::KnowledgeLoadingScreen` | `0x2aface0` | loading-screen tips menu | — |

`CSFadeSystem` update `FUN_140b3bfd0` walks a tree of fade plates and writes the composited RGB to
`self+0x20` and the final alpha to `self+0x2c` (clamped 0..1). Alpha ≈ 1.0 ⇒ screen fully covered — a
coarse "blacked out" signal (fires for cutscenes too, so use it only as a refinement).

## 2. Resolving the instances (the one real gap → do it live)

The FD4 singletons (`CSFadeSystem`, `CSFakeLoadingScreen`, `CSSessionManager`) and the `CSFD4LocationStep`
task are created **lazily through the FD4 reflection registry**, so their instance statics are NOT reachable
by static xref (the descriptor globals below are the reflection *runtime-class* nodes, not the live objects):

```
CSFakeLoadingScreen  runtime-class desc = DAT_143d74878 (er+0x3d74878)   (getInstance FUN_140bbf180)
CSSessionManager     runtime-class desc = DAT_143d7a4f8 (er+0x3d7a4f8)   (getInstance FUN_140cb2ab0)
CSFD4LocationStep    runtime-class desc = DAT_143d72578 (er+0x3d72578)   (register  FUN_140b41d00)
```

Resolve the LIVE instances on the Linux/Proton side (the DLL runs in-process; this repo's debug RPC has
**`find-what-accesses`**, the right tool):
- **Best:** during a real warp, arm find-what-accesses on `CSFD4LocationStep+0x48` (the phase getter
  `FUN_140b413c0` er+0xb413c0 reads it) → captures the live `step` pointer + its owning task-manager chain,
  giving a stable pointer path to pin.
- Or walk the **`CSTask`/`CSTaskGroup`** registry (RTTI-mapped, `FD4Singleton<CSTask>`) for the LocationStep
  task by its runtime-class `DAT_143d72578`.
- The **anchor** (`LocalPlayer==null`) needs **no** instance resolution — ship the watchdog on that first,
  add the `LocationStep+0x48` phase once its pointer path is pinned.

## 3. Target mapId (the "why")

`PLAYER_MAPID_SLOT` is already pinned (`src/re_signatures.hpp`, `windows_underground_player_pos` etc.). Log
it in the stall report. **Open (runtime) — the prompt's Q3:** confirm whether the player MapId updates at
load **start** (= the destination, ideal for the report) or only **after arrival**. If it only reflects the
arrived area, read the destination from the warp call instead: `goblin::warp::to_grace` /
`LuaWarp_01` already carry the target grace/bonfire id (`windows_grace_warppin_teleport_re_findings.md`) —
have MFG stash the last warp target when it drives a warp, and log that on a stall.

## 4. Failure/assert path (optional Q4)

Not separately pinned. A bad target most often **hangs in streaming** (no assert — the LocationStep phase
just stops advancing), which is exactly why the phase-unchanged timeout (§0) is the right detector rather
than an assert hook. The generic FD4 assert (`FUN_141eb97a0`, `…FD4Singleton.h`) fires for missing
singletons, not for a stuck stream.

## 5. Anchors (this build, er-relative)

```
LocalPlayer-null anchor   WorldChrMan [er+0x3d65f88] + 0x1E508 == null  ⇒ world not playable   (pinned)
menu gate                 CSMENUMAN_SLOT (pinned)  — exclude title/menu-only from "loading"
LocationStep phase        CSFD4LocationStep vtable er+0x2b6b750; ctor FUN_140b40c00 (er+0xb40c00, size 0xb8)
                          phase field step+0x48  (getter vtable[5] FUN_140b413c0; idle test vtable[4] FUN_140b41460 == -1)
fade "blacked out"        CSFD4FadeSystem vtable er+0x2b6a458; final alpha instance+0x2c (1.0 = opaque); update FUN_140b3bfd0
loading overlay           CSFakeLoadingScreen (Imp vtable er+0x2b803b8; desc DAT_143d74878)
session mgr               CSSessionManager (Imp vtable er+0x2b9a0c8; desc DAT_143d7a4f8)
target mapId              PLAYER_MAPID_SLOT (pinned)
reflection descriptors    CSFakeLoadingScreen DAT_143d74878 · CSSessionManager DAT_143d7a4f8 · CSFD4LocationStep DAT_143d72578
```

## 6. Deliverable status

**Shippable now:** a watchdog on the `LocalPlayer==null` anchor (+ CSMenuMan gate) + `PLAYER_MAPID_SLOT`
target → detects a stuck in-session load and logs {elapsed, mapId, stacks}. **Precise upgrade:** add
`CSFD4LocationStep+0x48` (phase index; `-1`=idle) once its live pointer path is pinned via find-what-accesses
on Linux — gives true phase + progress-stall detection. Runtime to confirm: the LocationStep instance path;
whether MapId is target-at-start or arrived-after (else stash the warp target). Class map + fields + the
phase getter are **DONE (static)**.
