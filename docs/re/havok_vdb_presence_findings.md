# Havok Visual Debugger (VDB) presence in ELDEN RING — recon findings (2026-07-05)

**Question (user):** before building an ImGui/ESP 3D overlay to visualise collision, does ER ship the
**Havok Visual Debugger** server? If so we could connect the official Havok VDB client and SEE the live
physics/collision world (shapes, bodies, our injected `add_collision` bodies) with no custom renderer.

## Finding: the VDB machinery IS compiled into `eldenring.exe` (NOT stripped)

`strings` on `eldenring.exe` (retail) — the Havok VDB / viewer / socket classes are present as RTTI:
- **Socket layer:** `hkSocket`, `ReaderAdapter@hkSocket`, `WriterAdapter@hkSocket` (the VDB TCP transport).
- **Process context:** `hkProcessContext`, `hkProcessContextBase`, `hknpProcessContext`
  (+ `QueryListener`, `ReportingBroadPhase` inner types) — the container the VDB attaches viewers to.
- **Physics viewers (draw the collision):** `hknpViewer`, `hknpViewerEx`, `hknpViewerColorScheme`,
  `hknpViewerColorModifier`, `hknpMultithreadedShapeViewerEx`, `HideFilteredBodiesEx`.
- **Server:** `hkbBehaviorServer` + the source path string `VisualDebugger\Server\hkbBehaviorServer.cpp`.
- **The hook point:** `hkSignal2<hknpProcessContext, hknpWorld>` and `hkSignal1<..., hknpWorld*>` — the
  viewer/context wires to the live `hknpWorld`. **We already hold the `hknpWorld` pointer** (`CSPhysWorld+0x08`,
  from the `add_collision` / heightfield-raycast RE).

Not spotted: a top-level `hkVisualDebugger` RTTI string or the default VDB port `25001`. So the VIEWER +
CONTEXT + SOCKET + SHAPE-VIEWER pieces are definitely linked; the top-level server orchestrator may be named
differently, partially compiled out, or simply not instantiated by the game.

## What this would give (and NOT give)
- **GIVES (dev tool):** stand up a VDB context + shape viewer over the live `hknpWorld` + open the socket →
  connect the **official Havok VDB client** → a full 3D view of the collision world: every collision shape,
  every body, AND our injected `add_collision` bodies + the far-terrain `hkxpwv` collision. A real
  verification/authoring tool for all the collision work (add_collision, walkable custom geometry, relief).
- **Does NOT give (player-facing):** the VDB is an EXTERNAL viewer app, not an in-game overlay. It does not
  render the greybox world for the player. So it complements — does NOT replace — the world-space ImGui/ESP
  overlay for the player-facing virtual world.

## Path (RE spike, Windows/Ghidra) — building blocks all present
1. Find the creation of an `hkProcessContext`/`hknpProcessContext` + how a `hknpViewer`/shape-viewer registers
   with it (the `hkSignal2<hknpProcessContext, hknpWorld>` connect). Reuse our live `hknpWorld`.
2. Find/instantiate the socket server (`hkSocket` + the server step) — the VDB listens on a TCP port; confirm
   the port (Havok default 25001) or trace it.
3. Per-frame `step` of the context/server (from the present thread or a game-thread hook, like the raycast).
4. Connect the version-matched **Havok VDB client** (ER's Havok is ~2018-2020 hknp — the VDB protocol is
   version-specific, so the client must match; sourcing the exact client is the main external hurdle).

## Risks / unknowns
- **Version-matched client:** the VDB wire protocol is Havok-version-specific → need the right client build;
  a mismatch won't connect. Main uncertainty.
- **Partial linkage:** viewer/context/socket RTTI being present ≠ the server is fully functional; some of the
  server may be dead-stripped to just what the game uses. Only a live spike proves it stands up.
- **Effort:** moderate-to-hard RE to wire context+viewer+socket+step; but every building block is in the exe
  and the `hknpWorld` handle is already ours.

## Verdict
Strong, cheap-to-check lead that came back POSITIVE: the VDB viewer/context/socket/shape-viewer are in the
binary. Worth a focused RE spike for a **dev-side** collision visualiser (verify add_collision / custom
walkable geometry / far-terrain collision via the official Havok tool). Orthogonal to the player-facing
ImGui/ESP greybox (that still stands for the virtual world's player-visible objects). See
`runtime_modding_framework_vision.md` #4 + `hknpworld_addbody_slot_re_findings.md` (the `hknpWorld` handle).
