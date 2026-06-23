# RE prompt — suppress the DISCOVERED-GRACE warp pin (the real draw-gate / container)

## Goal

Make the ImGui overlay the SOLE source of grace icons on the world map. The overlay
already draws every grace (config `grace_overlay`). The blocker: the game still
draws its own **discovered-grace warp pins**, so with the overlay on they DOUBLE.
We need to stop the native discovered-grace pin from drawing — WITHOUT touching the
player "you are here" dot, fog/fragments, player-placed markers, or objectives.

## What is already known / done (do NOT re-derive)

From `windows_native_grace_pin_manager_re_findings.md` (e4b3f6a) + the live
`[WARPPIN]` probe shipped in `src/goblin_inject.cpp` (`warp_pin_detour`):

- Discovered graces are **`CS::WorldMapWarpPinData`**, built by
  **`FUN_14088b7b0(this, out, WarpData* param_3, a4)`** at **`er+0x88b7b0`** — already
  hooked (`install_grace_suppression_hook`, config `grace_suppress_native`).
- At build time we can IDENTIFY a discovered grace: source entry `*(warpData+0x8)`,
  **state byte @ `src+0x1E`** (bits 0/1/2 = registered/discovered/visible; non-zero =
  a drawn grace), **iconId @ `src+0x08`** (region-coded 10001950/11001950/…). The
  `[WARPPIN]` log CONFIRMED this — phase A works.
- **Phase B FAILED (the open question):** zeroing the BUILT pin's **`pin+0xC`** (the
  copied state byte) via WriteProcessMemory does NOT suppress — the pin still draws,
  the overlay grace just stacks on top. So **`pin+0xC` is NOT the field the render
  path reads to decide whether to draw.**
- The category-pin manager `CSWorldMapPointMan [er+0x3D6E9B0]` (std::map `_Tree`@+0x390,
  `_Myhead`@+0x398, `_Mysize`@+0x3A0, node key+0x20 / value+0x28; show-predicate vt[1]
  `FUN_140a81450`) handles **WorldMapPointParam category** pins — NOT graces. We
  already suppress categories via `areaNo→99`. The grace warp pins are a SEPARATE
  system; do not assume they live in this map (e4b3f6a said they don't).

## What we need answered

The single question: **what actually gates whether a discovered-grace WarpPinData pin
draws, and what's the safe recipe to suppress ONLY discovered graces?** Concretely:

1. **Where does `FUN_14088b7b0`'s returned pin go?** Decompile the CALLER(s) of
   `FUN_14088b7b0` — the returned pin pointer is pushed into some container (vector/
   list/map on the WorldMapViewModel or the map dialog). Give that container's address
   path + element layout. That's the list to clear/filter for a clean suppression.

2. **What field decides draw?** `pin+0xC` is not it. Run find-what-READS on the pin
   object during the world-map render (or decompile the warp-pin draw walk) → identify
   the field(s) the renderer tests (a visible/enabled flag, an iconId, an alpha, a
   show-predicate vtable call). Report the offset + the value that means "don't draw".

3. **Is there a show-predicate for warp pins** (analogous to the category system's
   `vt[1] FUN_140a81450` that runs before insert)? If so, give the vtable slot / fn so
   we can force it to reject discovered graces (cleanest, non-destructive).

4. **Pick the safe suppression recipe** and confirm it leaves player-dot / objectives /
   fog / player-placed markers intact:
   - (a) **skip-build**: early-return / no-op `FUN_14088b7b0` for discovered graces
     (state `src+0x1E` bits set) — does the caller tolerate a null/empty return, or
     does it deref the result?
   - (b) **clear/filter the container** from (1) each map-tick.
   - (c) **flip the real draw field** from (2) on the built pin.
   - (d) **show-predicate** from (3).
   Which is safe (no crash, no collateral on the other pin types)?

## Deliverable

`windows_grace_warppin_suppression_re_findings.md`: the caller/container path, the
real draw-gate field (offset + suppress value), the show-predicate if any, and ONE
recommended suppression recipe with the exact offsets — droppable into
`warp_pin_detour` (replace the failed `pin+0xC` zero). Note any field that, if wrong,
would also hide the player dot / objectives / fog.

## Handles / RVAs (all known)
- builder `FUN_14088b7b0` @ `er+0x88b7b0` (already hooked). Args `(this, out, WarpData*, a4)`.
- source entry `*(warpData+0x8)`; state byte `+0x1E`; iconId `+0x08`.
- built pin: `+0xC` = copied state byte (proven NOT the draw-gate).
- category mgr `[er+0x3D6E9B0]` (+0x390 std::map, predicate vt[1] `FUN_140a81450`) — reference only, NOT graces.
- sibling mgr `[er+0x3D6F558]` (player-marker/objective, `FUN_140aba1a0` @ er+0x62426f) — must stay intact.
- App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Resolve live via AOB (ASLR), not static VA.

## Out of scope (separate, non-RE)
The "2% of GPU-mode graces land slightly off" is visible ONLY with `grace_gpu_sprite`
on → suspected the GPU-grace draw path (the `graceOffsetX/Y` × projection-factor
calibration scaffolding `s_grace_off_sx/sy` in map_renderer.cpp), NOT this native-pin
work. Investigate after suppression lands; the offset scaffolding gets deleted once the
overlay is sole (offset→0).
