# Windows-Ghidra + live-probe RE prompt — Havok VDB stand-up (OPTIONAL / secondary)

> **Priority: SECONDARY.** The recommended collision-visualisation path is the in-game ImGui/ESP overlay
> reading the hknp shapes we already RE'd (no Havok-version lock, in-game, player-capable) —
> `windows_world_to_screen_camera_re_prompt.md` + `runtime_modding_framework_vision.md` #4. Pursue THIS VDB
> path only as a "free 3D collision render if the version-matched client can be sourced". Do the w2s prompt
> first.

## Why (the lead)
`docs/re/havok_vdb_presence_findings.md`: `strings eldenring.exe` proved the Havok Visual Debugger machinery
is compiled into ER (NOT stripped): `hkSocket`(+Reader/Writer), `hk/hknpProcessContext`,
`hknpViewer`/`hknpMultithreadedShapeViewer`, `hkbBehaviorServer` + `VisualDebugger\Server\...cpp`, and the
`hkSignal2<hknpProcessContext, hknpWorld>` hook. If we can stand the VDB server up over the live `hknpWorld`
(we already hold it — `CSPhysWorld+0x08`, `hknpworld_addbody_slot_re_findings.md`) and connect the official
Havok VDB client, we get a FREE authoritative 3D view of the whole collision world incl. our `add_collision`
bodies — a dev verifier for all collision work.

## The GO/NO-GO risk to settle FIRST (cheap, do before deep RE)
**Version-matched client.** The VDB wire protocol is Havok-version-specific. Determine ER's Havok version
(hknp era ~2018-2020) — RTTI/strings often carry a version tag; also the `hknp*` class set narrows it. Then
check whether that era's Havok VDB standalone client is obtainable. **If no matched client exists, this whole
path is dead → stop and use the ESP route.** Settle this before the RE below.

## Questions (static Ghidra, only if the client risk clears)
1. Is any VDB context/server INSTANTIATED at boot, or only the classes linked? Find callers of the
   `hk/hknpProcessContext` + `hkSocket` ctors + `hkbBehaviorServer`. Retail usually links the classes but
   never starts the server → we'd create it ourselves.
2. **Creation recipe:** how to build a `hknpProcessContext` + a `hknpViewer`/`hknpMultithreadedShapeViewer`
   and connect them to the live `hknpWorld` via the `hkSignal2<hknpProcessContext, hknpWorld>`. Give the
   ctors + the register/connect call + arg layout.
3. **Socket server:** how the `hkSocket` server opens/listens (port — Havok default 25001? trace it) and
   accepts the client. Give the open/step calls.
4. **Per-frame step:** what must be `step`ped each frame (the context/viewer/server) to stream the world to
   the client. Cheapest hook (present thread vs a game-thread step).

## Live probe (Proton, dev RPC)
Behind a throwaway `vdb` RPC: create the context+viewer over the live `hknpWorld` (reuse the add_collision
world handle), open the socket, step it each present frame; then connect the matched VDB client and confirm
the collision world (incl. an `add_collision` test body) shows. Game must stay alive (SEH-guard the calls;
the socket/step must not block the present thread — thread off if needed).

## Verdict gate
Kill early if: no version-matched client (settle first), or the server is dead-stripped (only classes, no
functional socket/step). Else it's a strong free dev visualiser. Either way the player-facing path stays ESP.
