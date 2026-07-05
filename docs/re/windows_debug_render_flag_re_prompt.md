# Windows-Ghidra + live-probe RE prompt — FromSoft debug-render / wireframe flag

## Why (greybox job #2a — the cheap first check)
The runtime-modding framework distinguishes **three separate "greybox" jobs** (`runtime_modding_framework_vision.md`
§4 "SCOPE BOUNDARY"). This prompt is **job #2, option (a)** only:

> **Restyle the REAL base-ER render into greybox, KEEPING all systems** (physics/AI/combat/weapons/arts run
> untouched — the engine renders ITSELF, restyled; we mirror nothing and touch no system). Cheapest lever =
> **a FromSoft debug-render flag** if one exposes wireframe / untextured / flat / collision-display. **RE the
> flag → free if it exists.**

This is explicitly **NOT**:
- **Job #1** (draw OUR virtual-world objects via ImGui/ESP — that's `windows_world_to_screen_camera_re_prompt.md`).
- **Job #3** (hide meshes + redraw the whole scene as ImGui proxies — the WALL, unneeded).
- Job #2 options **(b)** post-process shader via present hook or **(c)** D3D12 PSO `FILL_WIREFRAME` override —
  those are separate, larger graphics-mod tracks. This prompt does the CHEAP scan FIRST so we know whether (b)/(c)
  are even necessary. **If a native flag exists, (b)/(c) are moot.**

The whole point: this is a **strings/RTTI/globals scan**, hours not days, with a binary outcome —
"ER ships a usable debug-render flag: yes/no". A yes is nearly-free greybox; a no closes the option cleanly and
routes to (b)/(c). Do the scan before anyone invests in a shader/PSO project.

## Deliverable
One of two verdicts, backed by evidence:
- **GO:** a resident global (byte/dword flag) or a small function whose flip makes the engine render its OWN
  geometry in a degraded/greybox style (wireframe, untextured, flat-shaded, collision-overlay, or
  no-post-process), **without breaking systems** (no crash, gameplay/physics/AI keep running). Give: the flag's
  address as an **AOB-anchored resolve** (sig on the code that reads it, not a raw RVA — repo doctrine), the
  value(s) and their visual effect, and the safe write point (which thread, when).
- **NO-GO:** the scan came up empty / every candidate is stripped or a no-op in retail. State what was searched
  and why it's exhausted, so we route to job #2(b) post-process or (c) PSO override with confidence.

## Context: what ER's renderer is (don't chase the wrong thing)
- ER is a **deferred D3D12 renderer** (FromSoft's Dantelion/GX engine). There is **no single "wireframe on"
  switch** guaranteed — retail is a shipping build, debug draw is often compiled out or gated. The realistic
  win is a **leftover debug-draw / draw-step flag**, not a full render-mode selector. Set expectations to
  "find a leftover flag", not "find a render-mode dropdown".
- The engine's per-frame render is submitted via the D3D12 command queue MFG already hooks
  (`hk_execute_command_lists`, `goblin_overlay.cpp`) — but the FILL/shader state lives inside immutable PSOs, so
  a render-STYLE change is either an internal engine flag (this prompt) or PSO interception (job #2c), never a
  present-hook tweak.

## Anchors / where to look (static Ghidra, `D:\ghidra_proj2\ER`, imagebase 0x140000000)
Reuse existing pins; do NOT re-map the render subsystem from scratch.
- **Render manager singleton** — `CANVAS_SINGLETON_RVA = 0x47ef360` (`DAT_1447ef360`, also referenced as the
  render/canvas manager in `goblin_worldmap_probe.cpp`; `RENDER_MGR_SLOT_RVA` in that file). Walk its vtable /
  fields for draw-mode or debug-draw state.
- **`GameRend@CS@@` / `GameRendCameraSet`** (er+0x680460) — the render-side camera/submit path
  (`windows_world_to_screen_camera_re_prompt.md`, `windows_freecam_re_findings.md`). Debug-draw toggles often
  sit near the render step that consumes the camera.
- **RTTI sweep** (`rtti_index.txt`) — the highest-yield move. Grep the RTTI/type names + string table for the
  usual FromSoft debug-render vocabulary:
  - Type/class names: `*DbgMenu*`, `*DebugMenu*`, `*DebugDraw*`, `*DbgDraw*`, `*DrawStep*`, `*GXDrawStep*`,
    `*DebugDrawer*`, `*DispMask*`, `*DrawParam*`, `CSDbg*`, `*Wireframe*`, `*WireFrame*`.
  - String literals near render code: `Wireframe` / `WireFrame`, `Fill`, `NoTexture` / `notex`, `Flat`,
    `DispMask`, `DrawGeom`, `DrawHit` / `Hitbox` / `DrawSphere` / `DebugSphere`, `DrawLow` / `DrawStep`,
    `CollisionDraw` / `hkVisualDebugger` render hooks, `bDraw*` boolean-flag names.
- **Known community lead — the hitbox/collision debug draw.** ER "hitbox viewer" mods flip an internal
  debug-draw enable (the Havok/hit-collision debug sphere/mesh renderer). That is the SAME class of "engine
  renders extra/degraded geometry from an internal flag" we want — even if it only draws collision, it PROVES a
  live debug-draw path exists and gives the flag's neighbourhood. **Find that flag first** (search the debug-draw
  vocabulary above + the hknp/Havok visual-debug hooks already noted in `havok_vdb_presence_findings.md`), then
  see whether adjacent flags cover mesh/texture/fill, not just collision.
- **Debug menu.** FromSoft retail often retains a **debug menu** structure (`CSDbgMenu`) whose entries are the
  toggles we want (draw steps, disp masks). If found, its backing globals ARE the flags — enumerate them.

## Questions to answer
1. **Does a resident debug-render / draw-step / disp-mask flag exist in retail ER?** If yes: address (AOB-
   anchored), type (byte/dword/bitfield), and the exact value→visual-effect mapping (which value = wireframe /
   untextured / flat / collision-only / no-post). If it's a bitfield or an array of draw-step booleans, list the
   bits/entries and what each disables.
2. **Is flipping it SAFE?** The whole value proposition is "engine renders itself, systems untouched." Confirm:
   no crash, physics/AI/combat keep running, it's a pure render-side flag (not a gameplay/streaming gate). Note
   the thread + timing for a safe write (present thread? game thread? any-time?).
3. **Coverage of the flag.** Does it degrade the WHOLE scene render (world geometry + characters) or only a
   subset (e.g. collision only, or only debug primitives)? A collision-only flag is still useful (a free in-game
   collision view) but does NOT satisfy job #2 "restyle the real render" — say which we got.
4. **If NO-GO:** enumerate what was searched (RTTI names, string literals, the render-manager vtable, the
   camera/render step, the hitbox-draw neighbourhood) and the negative result, so job #2(b)/(c) proceeds without
   re-treading this ground.

## Live probe (Proton, dev RPC) — the acceptance test
Behind a throwaway `dbgrender_probe` RPC (mirror the `goblin_geom_move.cpp` / hf-probe SEH-guarded read/write
shape; the scissor recon in `goblin_overlay.cpp` is a good sibling for "flip a render-side flag, observe, revert"):
- `dbgrender_probe read` — dump the candidate flag(s) current value.
- `dbgrender_probe set <flag> <val>` — write under SEH, then **`screenshot`** (existing RPC) to capture the
  frame. Compare against a baseline screenshot: the world should render in the degraded/greybox style, and the
  game must stay alive (take a step, swing — verify no freeze/crash). Revert and screenshot again to confirm
  it's a clean toggle.
- **Oracle:** a KNOWN busy view (stand in a lit textured area facing geometry) so wireframe/untextured/flat is
  visually unambiguous. For a collision-only flag, stand where a wall's collision differs from its mesh.
- Acceptance = a screenshot pair (flag off / flag on) showing the engine's OWN geometry restyled, game still
  interactive. That's the GO. No safe flip found after the scan is exhausted = the documented NO-GO.

## Sequencing / gate
- **Cheap, do it standalone.** This is a scan with a binary outcome; it is NOT on the M5 (native-map cull) or
  w2s3d critical path — run it independently on the Windows/Ghidra box whenever there's a slot.
- **It gates nothing and blocks nothing.** A GO makes greybox job #2 nearly free; a NO-GO just points at the
  (b)/(c) fallbacks. Either way it retires an open roadmap question for a few hours of RTTI/strings work.
- Write findings to `docs/re/windows_debug_render_flag_re_findings.md` and update the RE coverage map
  (`docs/re/README.md`).
