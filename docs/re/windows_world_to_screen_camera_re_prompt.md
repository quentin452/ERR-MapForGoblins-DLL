# Windows-Ghidra + live-probe RE prompt — 3D world-to-screen (camera view-projection matrix)

## Why (the single unblocker for the ImGui/ESP 3D overlay)
The runtime-modding framework's **virtual worlds render via an in-game ImGui/ESP overlay** (decision:
`runtime_modding_framework_vision.md` #4). The pipeline is: read an object's world `(X,Y,Z)` → **project to
screen pixels** → `ImDrawList` box/diamond/line/wireframe. Everything on that path exists EXCEPT the
projection: we have the 2D MAP `w2s` (world XZ → canvas) but NOT a 3D **gameplay** world-to-screen. This one
primitive unblocks: the virtual greybox world (player-facing), an in-game collision wireframe (draw the hknp
shapes we already RE'd — a no-VDB-version-lock alternative to the Havok Visual Debugger), and the freecam's
overlay HUD. **Get the live camera view-projection matrix and this whole class of feature opens.**

## Deliverable
A **live world-to-screen** the DLL can call every present frame:
`w2s3d(worldXYZ) -> (screenX, screenY, visible)`, where
`clip = ViewProj * vec4(world,1)`, `visible = clip.w > 0`, `ndc = clip.xyz / clip.w`,
`screen = ((ndc.x*0.5+0.5)*W, (1 - (ndc.y*0.5+0.5))*H)` (confirm NDC Y sign + the DX clip convention live).
We already have the viewport `W,H`. **The one unknown = where the live 4×4 View-Projection matrix (or the
View + Projection pieces) lives, and how to resolve it each frame.**

## Anchors (reuse the freecam recon — do NOT re-map the camera subsystem)
From `docs/re/windows_freecam_re_findings.md` + `rtti_index.txt` (er base 0x140000000):
- **`GameRendCameraSet@GameRend@CS@@`** (er+0x680460…) — the RENDER-side camera set that "consumes the final
  view matrix". **Most likely holder of the combined View-Proj (or View + Proj) the renderer submits to the
  GPU.** Primary target.
- **`CSCameraImp@CS@@`** (FD4Singleton, ctors er+0x3bae40 / er+0x3bad20) — camera manager; active camera
  global `DAT_143d6b880`, view transform at `[*(+0x10)+0x18]+0x10` (the camera WORLD transform, per freecam).
- Per-frame camera step `FUN_140766980`; transform write-point `FUN_1407650a0`.

## Questions to answer (static Ghidra)
1. **Find the VIEW-PROJECTION matrix (preferred) or the View + Projection matrices** the renderer uses this
   frame. Best lead: trace what `GameRendCameraSet` (er+0x680460) stores/produces — a `float[16]` (row- or
   column-major?) that flows toward the D3D12 constant-buffer upload. Give: the field offset from a
   resolvable base (the `GameRendCameraSet` instance, or `CSCameraImp` singleton → …), the layout
   (mat4, and whether it is View, Proj, or ViewProj), and row/column-major.
2. **Resolve chain + AOB.** How does the DLL reach that matrix each frame from a stable anchor? Prefer an AOB
   on the code that reads/writes it (pin the sig, not a raw RVA — repo doctrine). If it hangs off the
   `CSCameraImp` singleton, give the singleton AOB (like `MSG_REPOSITORY` / `WORLD_GEOM_MAN_SLOT`) + the
   offset chain. Note thread-safety: is it safe to read on the PRESENT thread (where our overlay draws), or
   only mid-frame? (The map raycast reads Havok on the game thread; the view matrix is likely present-safe as
   a read, but confirm it isn't being written concurrently mid-read.)
3. **Conventions:** near/far, FOV source, NDC handedness, clip.w sign for "behind camera", and whether the
   matrix already includes the viewport or only clip-space (we apply the viewport ourselves). Confirm the
   Y-flip for DX screen space.
4. **Does it update for the debug/free camera too?** If freecam (Route 2) overrides the camera, does the
   same ViewProj reflect the new view (so the overlay stays correct while flying)? It should if it's the
   render-consumed matrix — note if not.

## Live probe (Proton, dev RPC) — the acceptance test
Behind a throwaway `w2s_probe` RPC (mirror `goblin_geom_move.cpp` / the hf probe style):
- Read the candidate matrix, project a KNOWN world point and compare to where it actually appears on screen:
  - **Best oracle: the player.** We have `get_player_map_pos` / the live ChrIns world XYZ (`LocalPlayer+0x6C0`
    frame). Project the player's world pos → it must land on the player's feet on screen. Draw a debug ImGui
    dot at the projected pixel; it should stick to the character as the camera moves/rotates.
  - Project a few offsets (player + (0,+2,0) = above the head; player + (5,0,0)) → the dots must track in 3D
    perspective (converge with distance, move correctly on yaw/pitch).
- Confirm behind-camera cull (`clip.w <= 0` → skip) and edge behaviour. Confirm it holds across a full
  camera orbit + zoom.
- Success = the projected dot locks to the world point through camera motion → the matrix + convention are
  correct → ship `w2s3d` as a `GOBLIN_RENDER_API` the overlay/panels call.

## Notes
- This is moderate, well-trodden RE (every ESP/overlay mod does exactly this) — the ER-specific part is only
  WHERE the matrix lives + the resolve chain. Shares the camera subsystem with freecam, so do them together
  if convenient, but the deliverables differ: freecam WRITES the camera transform; this READS the ViewProj.
- Once shipped, it feeds `runtime_modding_framework_vision.md` #4 (virtual greybox world), an in-game hknp
  collision wireframe (reuse the `hknpCompressedMeshShape` / `hknpBoxShape` RE — no Havok-VDB version lock),
  and any 3D-anchored HUD.
