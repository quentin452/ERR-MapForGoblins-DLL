# RE findings — placement hook to capture clean `(world_in → render_out)` pairs

Answers `docs/windows_marker_affine_hook_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
script `re_v56`) pinning the icon-placement call site so the world→render affine
`render = M·world + T` can be fitted from source-exact pairs. App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only.

## TL;DR

- **`world_in` capture site is fully pinned and NON-VMP.** Hook the `CALL
  thunk_FUN_1457cd3df` in reconcile `FUN_140a832a0` at **RVA `0xa839a6`**; at the call
  **RCX = `CSWorldMapPointIns*` (`point`)**, RDX = ctx. The baked param row is at
  **`point + 0x80`** (`areaNo` u8, `gridXNo` u8, `gridZNo` u8, `posX/posZ` f32) →
  `world = (gridXNo·256+posX, gridZNo·256+posZ)`.
- **`render_out` is NOT a plain field on `point`.** The ctor's `point+0xa0/+0xe0` vecs are
  a **color/UV LUT** (`FUN_1401899c0` — returns constants like 1.0 / 0.97, not coords), and
  `point` has no render-pos field. The render position is written by the VMP thunk onto the
  icon's `Scaleform::Render::Matrix2x4<float>`. So `render_out` must come from **(a)** an
  in-thunk matrix-write breakpoint (CE can bp into VMP), or **(b)** the cursor snap
  cross-check, which is *exact when the reticle is snapped on the icon* (the snap target IS
  the icon render pos — so the hover recipe is not actually noisy if you let it snap).

## The hook site (disassembly, reconcile `FUN_140a832a0`)

```
0xa8397e  MOV   RDI, [R15 + 0x398]      ; R15 = CSWorldMapPointMan; +0x398 = built-icon map
0xa83985  MOV   RBX, [RDI]              ; RBX = first RB-tree node
0xa83988  CMP   RBX, RDI                ; node == sentinel? -> done
        ... per-node loop ...
0xa83990  MOVUPS XMM1, [RBX + 0x20]     ; node key@+0x20, value@+0x28
0xa83994  PSRLDQ XMM1, 0x8             ; -> node+0x28 (the map value)
0xa83999  MOVQ  RCX, XMM1              ; RCX = CSWorldMapPointIns*  == `point`
0xa8399e  TEST  RCX, RCX
0xa839a1  JZ    skip
0xa839a3  MOV   RDX, R13               ; RDX = ctx (param_2)
0xa839a6  CALL  thunk_FUN_1457cd3df    ; <-- HOOK HERE. RCX=point, RDX=ctx
```

**AOB for the call site** (unique; `66 48 0F 7E C9` = `MOVQ RCX,XMM1`, ends at the `E8` CALL):
```
66 48 0F 7E C9 48 85 C9 74 08 49 8B D5 E8
```
`E8` is the `CALL rel32`. Hook = AOBScan this, the call is at `found+13`. Read `point` from
RCX. (Same icon update is also driven from build `FUN_140a82a80`; the reconcile site above
fires every frame the map is open and is the convenient one.)

## `world_in` — read at the hook (NON-VMP, exact)

```
point   = RCX
row     = point + 0x80          ; baked WORLD_MAP_POINT_PARAM_ST (mod's injected layout)
areaNo  = byte  [row + 0x00]    ; page disambiguation (after the game's legacy-conv)
gridX   = byte  [row + ...]     ; WORLD_MAP_POINT_PARAM_ST field offsets per the mod's paramdef
gridZ   = byte  [row + ...]
posX    = float [row + ...]
posZ    = float [row + ...]
world   = (gridX*256 + posX, gridZ*256 + posZ)
```
(Use the mod's `WORLD_MAP_POINT_PARAM_ST` offsets — the same struct it injects — for the
exact field positions within `row`.) Read the MapId via vt[4] `FUN_140a81140` if a page
needs disambiguation beyond `areaNo`.

## `render_out` — two ways (the VMP boundary)

`thunk_FUN_1457cd3df` (entry `0xa805e0`) jmps into the VMProtect region; the matrix write is
there. Options, in order of cleanliness:

1. **In-thunk matrix write (exact).** Bp `0xa805e0`, step until the two adjacent `float`
   stores that land render-space values (hundreds … ~10496, ≈0.5× world) onto the icon's
   `Matrix2x4<float>` translation `(tx, ty)`. Conditional-log there, paired with `point`
   captured at `0xa839a6` (same frame). This is the brief's preferred capture.
2. **Cursor snap cross-check (already exact when snapped).** The reticle snap target equals
   the icon render pos, so cursor `+0x104/+0x108` for a snapped grace IS `render_out`. Use
   `tools/cheat_engine/MapForGoblins_mapspace.CT` (NUMPAD 0) — pairing it with the param-row
   `world_in` (which you now read exactly from `point+0x80`, no guessing which grace) already
   gives clean pairs. The "hover noise" the brief assumes only applies to *un-snapped* hover.

Reading `point` post-call does NOT yield `render_out` — `point` carries no render field
(only the color LUT + the param row + sub-objects). The render pos is on the icon node.

### Post-call read is NOT statically resolvable (re_v58, confirmed)

The thunk at `0xa805e0` is a **bare `JMP 0x1457cd3df`** straight into the VMProtect region —
it does **not** load `point->icon` in non-VMP code first. The point ctor caller that NEWs
the instance + creates its icon is at `0xa82d09`, in the **un-analyzed VMP gap** (Ghidra
never made it a function). So `point→icon` and the icon→`Matrix2x4` translation offset
**cannot be lifted statically** — they live inside VMP. The post-call plain-memory read
(and a fully in-DLL self-calibration) therefore need the **live in-VMP step** (bp `0xa805e0`,
follow the jmp, find the two-float matrix store), or fall back to the **cursor snap
cross-check** for `render_out`.

**Recommended capture path (no VMP step needed):** hook `0xa839a6` for an exact `world_in`
(`point+0x80`, the param row), and take `render_out` from the **snapped cursor**
(`+0x104/+0x108` — exact when the reticle is on the icon). The hook removes the "which grace
am I on?" ambiguity; the snap removes the hover error. That yields clean pairs with only the
already-shipped CT, no breakpoint into VMP.

### Param row field layout (at `point+0x80`)

Relative layout (mod's `WORLD_MAP_POINT_PARAM_ST`, `…/from/paramdef`): `angle` (f32) then
**`areaNo` u8, `gridXNo` u8, `gridZNo` u8, pad u8, `posX` f32, posY f32, `posZ` f32** —
i.e. from the `areaNo` byte: `gridXNo=+1`, `gridZNo=+2`, `posX=+4`, `posZ=+12`. Match the
`areaNo` byte in the live row at `point+0x80` (small 60/61/12/40-43) to anchor the base.

## Deliverable handoff

The overlay already fits `M` (shared) + per-page `T` from collected pairs
(`g_aff`/`solve_affine` in `goblin_overlay.cpp`). To bake exact constants:
- capture ≥3 pairs/page (60, 61, 12, 40-43) via option 1 or 2 above,
- `lstsq` per page (solver in `marker_mapspace_CT_recipe.md`),
- confirm `M` shared (hypothesis: 90° axis-swap @ 0.5: `a≈0, b≈±0.5, c≈±0.5, d≈0`), bake
  `M` + `T[page]`, replace the runtime solve.

## Do UNDISCOVERED markers have a readable render position? (re_v59)

**Static answer: almost certainly NO → the `M·world+T` bake is required.** Evidence:

- **Visibility is build-time + event-flag (discovery) gated, not a draw-time alpha.** The
  show-predicate `vt[1] FUN_140a81450` *registers* the point (`FUN_140d0cb20(PointMan+0x158,
  …)`) only when its gate passes, and the prior live-refresh RE confirmed `vt[1]` is **not
  re-run in the per-frame reconcile** — so it gates *whether the point is built/registered*,
  not per-frame drawing. The per-row predicate `FUN_140d58470` evaluates the row's display-
  group flag via `_VerifyEnableEventFlag`/`_VerifyDisableEventFlag` (`0xd58640`/`0xd58550`)
  — i.e. discovery/event flags decide inclusion.
- The unconditional icon-update over `+0x398` only positions points **already in** that map;
  it does not build entries for gated-out (undiscovered) markers.
- Architectural corroboration: the mod has to **inject `WorldMapPointParam` rows** to show
  undiscovered graces at all (and gates on `require_map_fragments`) — if the game already
  built+positioned every marker and merely hid undiscovered ones, neither the injection nor
  this whole `M·world+T` effort would be needed.

So the engine builds/positions only the markers that pass the discovery+fragment gate;
undiscovered ones (the overlay's reason to exist) have **no render position to read** → the
overlay must compute them via `render = M·world + T`.

**Definitive live confirmation (the brief's count — user runs):** with the map open on a
page, count entries in `CSWorldMapPointMan+0x398` vs on-screen icons. If `+0x398` has **no**
entry for an undiscovered grace → confirmed NO (matches the static read). If it *does* carry
undiscovered entries with a valid `Matrix2x4` translation (render range) → that would flip
it to YES and let us skip the math; the static evidence says not to expect that.

**LIVE-CONFIRMED NO (2026-06-20).** The `render_finder` CT (`tools/cheat_engine/
MapForGoblins_render_finder.CT`, vtable-scans all 3 instance classes — `CSWorldMapPointIns`
`0x2b487a8`, `CSWorldMapDiscoveryPointIns` `0x2b48440`, `CSWorldMapReentryPointIns`
`0x2b48d28`) found only **4 built instances total** on an open overworld page
(`Point=2 Discovery=1 Reentry=1`). So the engine builds an instance (and a render position)
only for the handful of **discovered/given** markers — **undiscovered markers are not built
→ no render position to read → `render = M·world + T` is mandatory.** Corollary: the
finder/calibration needs MANY built instances with known world, so it must be run with the
mod's **marker injection ON** (show_all / many categories) — the injected rows are what the
engine builds + positions, giving the `(world → render)` sample set to fit `M`/`T`.

`point+0x80` is a **pointer** to the `WorldMapPointParam` row (ctor inits it to 0), not the
inline row — read `[point+0x80]` then the row fields. (The 4-instance run was too few +
heterogeneous to locate a consistent row, hence the finder needs the injected-marker set.)

## Handles / AOBs

- reconcile `FUN_140a832a0`; **hook call @ `0xa839a6`** (AOB above), RCX=point, RDX=ctx.
- icon update `thunk_FUN_1457cd3df` entry `0xa805e0` (jmp → VMP; matrix write inside).
- `CSWorldMapPointIns`: param row `point+0x80`; color LUT `FUN_1401899c0`; MapId vt[4]
  `FUN_140a81140`; vtable `0x142b487a8`.
- `CSWorldMapPointMan` static `0x143d6e9b0`; built-icon map at `+0x398` (key@node+0x20,
  value@node+0x28 = the `CSWorldMapPointIns*`).
- Cross-check: cursor render `+0x104/+0x108` (vtable scan `0x142b29a90`), WorldMapArea
  `[cursor+0xF0]` fullRect `+0x350` `[0,0,10496,10496]`, pan `+0x378`, zoom `+0x380`.
- Offsets version-specific; resolve point-man/instances by static + vtable (patch-robust).
```
