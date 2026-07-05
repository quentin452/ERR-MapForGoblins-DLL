# Virtual-world 3D backend + procedural-object plan

Status: **DESIGN, not started (2026-07-05).** Captures the user's insight that the virtual/greybox worlds
(runtime-modding vision #4, job #1) do **not** have to be pure-ImGui-2D — a **mod-owned 3D backend** rendering
**procedural** geometry gets both wins the user named:
1. **3D parfaite** — real meshes, shading, depth occlusion (not flat billboards).
2. **Aucun objet lourd à charger** — geometry is *generated at load from tiny TOML params*, so **no `.flver`/
   `.hkx`/`.tpf` assets, no streaming, no VFS, no MSB-write, no FLVER authoring**. This **sidesteps the entire
   create-new-content RE frontier** (the coverage map's MSB-write + FLVER-authoring walls, `docs/re/README.md`).
   A greybox world = DATA (TOML) → realized by CODE (proc-mesh gen + Havok box collision) → drawn by the mod's
   own D3D12 backend. FromSoft's asset system is never touched → mod-agnostic + light by construction.

## The two render backends (per object, `render.backend`)
- **A — `imgui`** (2D draw-list, projected each frame via w2s3d): billboards / wireframe / labels. No shaders,
  flat, no depth (draws over ER geometry). Cheap. Best for **ESP markers on the real world**, not a walkable world.
- **B — `mesh3d`** (mod-owned D3D12): real 3D draws submitted into ER's swapchain via the `ID3D12Device` +
  command queue + Present hook the overlay **already holds** (`goblin_overlay.cpp`). Real geometry + shading +
  depth. The right tool for a **walkable virtual world**. On Linux these draws go through vkd3d-proton like ER's.

## Object schema (TOML) — core + render
Core (both backends): the object EXISTS. Render block differs. New file e.g. `<world>/objects.toml`, parsed with
the Proton-safe `TOML_EXCEPTIONS 0` + `parse_file` path (see `toml-parse-file-proton-bug.md`).
```toml
[[object]]
id        = "plat_01"
world     = "dev_arena"                    # goblin_virtual_world id/name
pos       = [1000.0, 50.0, 1000.0]         # world XYZ (the frame add_collision/warp use)
rot       = [0.0, 0.0, 0.0]                # euler deg
kind      = "platform"                     # platform|wall|ramp|beacon|trigger|spawn|dummy
collision = { shape = "box", size = [4.0, 0.5, 4.0] }   # Route D hknpBoxShape, walkable (already RE'd)

# --- backend A (ImGui 2D) ---
[object.render]
backend = "imgui"
shape   = "box_wireframe"   # diamond|circle|box_wireframe|line|billboard|label
color   = "#40C0FFFF"; thickness = 2.0; depth = false; label = "Platform 1"; size_src = "collision"

# --- backend B (real 3D, procedural) ---
[object.render]
backend   = "mesh3d"
primitive = "box"           # (see procedural tiers below)
size      = [4.0, 0.5, 4.0] # WORLD units — SAME as collision => ONE geometry source
material  = "greybox"       # flat|greybox|wireframe|unlit_color
color     = "#40C0FFFF"; depth = true
```
**3D unifies collision + render** (one `size` drives the walkable box AND the mesh) — cleaner than ImGui's
decoupled (collision box + separate 2D glyph).

## ★ The procedural-3D track — NOT locked to the primitive list
The 7 built-ins are the starter kit. Custom procedural geometry, all **no external mesh file**, in tiers:
1. **Parametric primitives** — `box | plane | sphere | cylinder | capsule | wedge | grid` (from params).
2. **Composite / CSG** — union/stack of sub-primitives; one object = a list. **Covers ~90% of greybox** (levels
   ARE stacked boxes): a castle = boxes, a bridge = box + arch.
   ```toml
   [object.render]
   backend = "mesh3d"; material = "greybox"
   parts = [ {primitive="box", size=[10,1,3], pos=[0,0,0]},
             {primitive="box", size=[1,4,3], pos=[-4.5,2,0]},   # wall
             {primitive="cylinder", radius=0.5, height=4, pos=[4.5,2,0]} ]  # pillar
   ```
3. **Procedural generators** (parametric shapes beyond the basics):
   - `extrude` a 2D polygon footprint → arbitrary prism (hex tower, L-platform, any floorplan).
   - `lathe`/revolve a profile → columns, vases, domes.
   - `heightfield` grid displaced by a function or a data array → terrain/hills (reuse the relief heightfield).
   - `sweep` a cross-section along a spline → roads, rails, pipes.
   - `superquadric` → rounded boxes/blobs.
   ```toml
   [object.render]
   backend = "mesh3d"; primitive = "extrude"; height = 6.0
   polygon = [[0,0],[4,0],[4,2],[2,4],[0,2]]   # footprint -> prism
   ```
4. **Inline mesh data** — raw `verts` + `tris` in the TOML (a tiny custom mesh, still zero external file).
5. **Custom named generators** — register a C++ proc-gen fn, expose it as a `primitive` name (`"spiral_stair"`,
   `"arch"`, `"torus"`); extensible without new file formats.

So "custom 3D" = yes, arbitrary — via CSG + generators + inline data, never a `.flver`.

## Case 1 vs Case 2 — the w2s3d dependency (important for sequencing)
- **Case 1 — objects overlaid on ER's REAL world** (ESP / a virtual object floating in the live game): must
  align with ER geometry → needs ER's live **view-projection** = the **w2s3d matrix** (currently BLOCKED on the
  render-rebase origin, `windows_world_to_screen_camera_re_findings.md` §live-calibration). Whether ImGui or 3D,
  you feed that matrix (CPU-project for ImGui, GPU constant-buffer for 3D).
- **Case 2 — a STANDALONE virtual/dev dimension** (player warped to an empty reserved map, the mod renders the
  WHOLE scene): the mod builds its **OWN camera** from the player pos + yaw (already read) → **no ER view-proj
  needed → the w2s3d blocker DISAPPEARS.** ⇒ the walkable-world path is **NOT** on the w2s3d critical path.

## What exists vs what's needed
- **Have:** `add_collision` (Route D walkable `hknpBoxShape`, live-proven), the D3D12 device/queue/Present hook
  (`goblin_overlay.cpp`), the virtual-world registry (`goblin_virtual_world`), player pos/yaw, `warp`,
  `get_player_dimension_area` (active-world follow).
- **Need:** (a) the mod-owned D3D12 **3D render backend** (PSO/shaders/root-sig/vertex+index buffers, one flat/
  greybox shader); (b) the **proc-mesh generator library** (primitives → CSG → generators → inline); (c) the
  **objects TOML schema + realizer**; (d) **case-1 only:** finish the render-rebase-origin RE for w2s3d.

## Realizer-logs-gaps (from the dev-dimension idea, HANDOFF)
The TOML realizer should **log every field / primitive it can't yet honor** → the missing-primitive checklist
writes itself live in-game as you author worlds (matches the "dev dimension walking skeleton" idea).

## Sequencing
1. Backend B skeleton: draw ONE hardcoded greybox box in the swapchain via the mod's D3D12 (proves the backend).
2. Own-camera (case 2) from player pos+yaw → box sits in the world as you move (no w2s3d).
3. Proc-mesh tier 1 (primitives) + the objects TOML + realizer. Then CSG (tier 2) = most greybox unlocked.
4. Collision+render unified from one `size`; walk-on via `add_collision`.
5. Later: generators (tier 3), inline (4), custom (5); and case-1 overlay once the w2s3d rebase lands.
