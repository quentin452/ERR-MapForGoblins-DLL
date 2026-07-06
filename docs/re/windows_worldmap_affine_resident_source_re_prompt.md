# RE brief — RESIDENT source of the world→map-space converter affine (project with the native map NEVER opened)

**✅ ANSWERED (static, 2026-07-06) → `windows_worldmap_affine_resident_source_re_findings.md`.** Hypothesis
**A**: the base affine source is **exe-invariant, no mod-changeable param** — bias `{128,128}` + scale `1.0`
are `.rdata` constants, origin is a `.data` global that a single init fn just **zeroes** (no param writer),
area/grid keys are ctor immediates; only the legacy fold is a param (already `legacy_fold`). Remaining =
the empirical map-closed validation (Linux: build the converter off-VM, call `FUN_140876140`, `du/dv==0` ⇒
drop the prime). Brief kept below for the task detail.

**Goal.** Let MapForGoblins project ANY `(area, gridX, gridZ, posX, posZ) → map-space (u,v) + page`
**with the native ER world map never opened this session** — no `WorldMapViewModel`, no prime, mod-agnostic.

**Why this matters (the blocker it removes).** The vmap-only migration wants the Virtual Map to be the
sole surface. Today the live projection (`goblin::worldmap_probe::project`, `src/goblin_worldmap_probe.cpp`)
calls the engine through the **`CS::WorldMapViewModel`**, which only resolves **after the native map is
opened at least once** (`find_view_model` caches from `g_active_cursor`; `proj` RPC returns
`err converter unresolved` until then). That forces either (a) leaving the native map menu open (it then
eats gamepad/mouse input → ER's own map buttons trigger under our cover), or (b) a "silent prime"
(open+close the native map once at boot — live-proven to work 2026-07-06, but it flashes the native map and
is a hack). If the affine's **source data is resident without the map**, we drop the VM dependency entirely:
project map-closed, own the input cleanly, no prime, no flash.

---

## What is ALREADY SOLVED (do NOT re-derive — build on it)

From `windows_world_to_mapspace_projection_re_findings.md` (§1–§7) + `world_map_projection_re_findings.md`:

- **The projection is a tiny affine per "converter" entry.** Converters live inline in the VM:
  `converters = WorldMapViewModel + 0xF8` (array, **stride 0x30, up to 8 slots**), `count = VM + 0x280`,
  page id per slot = `(&DAT_142ad82f8)` bytes `[00 01 0a …]` = `{0 overworld, 1 underground, 10 DLC}`.
- **Converter entry (0x30 bytes)** — pinned from builder `FUN_140876100` + reader `FUN_140876140`:
  ```
  +0x08 u32  key   (+0x09 u8 gridZbase, +0x0A u8 gridXbase, +0x0B u8 area)
  +0x0C f32  originX      +0x14 f32 originZ
  +0x18 f32  biasX        +0x1C f32 biasZ
  +0x20 f32  scale
  +0x28 ptr  legacyConvNode  (WorldMapLegacyConvParamGroup node; 0 = no fold)
  ```
- **The math (`FUN_140876140`, exact):** fold legacy dungeon via `+0x28` if present, require
  `(id>>24)==area`, then `mapX = (worldX-originX)*scale + biasX`, `mapZ = -(worldZ-originZ)*scale + biasZ`.
- **The values are KNOWN for ERR/vanilla:** overworld (area 60), DLC overworld (61) and base-UG (12) all
  share `scale 1, originX 7168, originZ 16384, biasX 128, biasZ 128` → our baked `−7040 / +16512`. DLC
  overworld == overworld; only the *page byte* differs. The one genuinely mod/page-variable step, the
  **legacy-dungeon fold, is `WorldMapLegacyConvParam` — a REGULATION param, already resident map-closed and
  already consumed live by `goblin::legacy_fold` (`src/worldmap/legacy_fold.cpp`).**
- **Callable engine entry:** `FUN_1408877d0(VM,&outXZ,&packedId,&worldLocal)` (loop wrapper) /
  `FUN_140876140` (per-slot). `worldLocal` is AREA-LOCAL `{px,0,pz}`; `packedId=(area<<24)|(gx<<16)|(gz<<8)`.

So the affine reduces to **~5 constants per converter + a resident param for the fold**. The fold source is
already resident. The open question is only the **origin/bias/scale/key source**.

---

## THE QUESTION (the only unknown)

**Where do the per-converter `origin/bias/scale/key` values come from when the `WorldMapViewModel` is
constructed, and is that source resident with the native map CLOSED (ideally never opened)?**

Two hypotheses — decide which, with evidence:

- **(A) Exe-baked constants.** The VM ctor `FUN_1408855b0` and/or the converter builder `FUN_140876100`
  write the slots from **immediates / a static const table in `eldenring.exe`**. If so, the affine is
  **mod-invariant** (mods ship regulation + assets, not an exe patch) → we can hardcode/read the static and
  project map-closed for ANY mod, forever, with no VM. (Our baked `−7040/+16512` would then be provably
  complete for the base affine; only `WorldMapLegacyConvParam` varies, already handled.)
- **(B) Param / resource-loaded constants.** The ctor reads the slots from a **regulation param** (e.g. a
  `WorldMap*Param` — check `WorldMapLegacyConvParam`'s neighbours, `WorldMapPlaceNameParam`,
  `WorldMapPointParam`, any `WorldMapConvParam`-like table) or a map resource/bnd loaded at boot. If so,
  **read that param directly** (resident map-closed like LegacyConv) → mod-agnostic and no VM.

Either answer unblocks the goal. (A) → read the static / keep the baked affine as provably-complete;
(B) → read the param. Both let us build the 8-slot converter array off-VM and reuse `FUN_140876140`, OR
just replicate the affine.

---

## RE TASKS (Windows Ghidra primary — project `D:\ghidra_proj2\ER`, app 2.6.2.0)

1. **Decompile the VM ctor `FUN_1408855b0`** (it already loads the LegacyConv group per §3 of the findings).
   Find where it fills `VM+0xF8 + i*0x30` slots — the writes to `+0x0C/+0x14/+0x18/+0x1C/+0x20/+0x08`.
   Is each field an **immediate** (→ hypothesis A) or a **load from a pointer/param table** (→ B)?
2. **Decompile the builder `FUN_140876100`** — same question at the point it stamps a converter entry.
   Trace its inputs (args / globals) back to the source. If it reads a struct, identify that struct's
   origin (param registry `SoloParamRepository` lookup? a `DAT_…` static? a file/bnd handle?).
3. **If a param:** get its **name + row/field offsets** for `origin/bias/scale/area/grid`. Confirm it is in
   the regulation and resident map-closed (same class as `WorldMapLegacyConvParam`, resolvable via the
   project's `from::params::get_param<T>` path).
4. **If exe-baked:** get the **static table address / the immediates per slot** and how the slot count (8)
   + `key`/`area`/page mapping is chosen. Confirm they are `.rdata` constants (mod-invariant).
5. **Note DLC/UG slots:** confirm whether the DLC(10)/UG(1) slots come from the SAME source and are present
   in it BEFORE their map is opened (findings say they only populate the VM when that map opens — but the
   SOURCE they populate FROM may be resident all along; that is the whole point).

## Runtime verification (Linux daily-build box is fine here — in-DLL, no big external scan)

- **Residency probe:** with the native map **NEVER opened this session**, can we read the source?
  - If (B) param: read it via `from::params` and dump the candidate affine rows to the log; compare the
    overworld row to the known `scale 1 / origin 7168,16384 / bias 128`.
  - If (A) static: `mem_dump <er+static>` (RPC) map-closed → expect the constant table.
- **End-to-end map-closed proj:** build an 8-slot converter array in our own memory from the found source,
  then call `FUN_140876140` per slot (we already resolve it, `WORLDMAP_PROJ_POINT`) — with the map never
  opened. Compare `(u,v)` to a reference captured with the map open (`proj 60 42 36 → u=3712 v=7296` on the
  dev save). **du/dv == 0 ⇒ the VM is unnecessary.** Extend `tools/rpc_tests/test_converter_residency.py`
  with a "never-opened" variant.

## Deliverable

Write `docs/re/windows_worldmap_affine_resident_source_re_findings.md`: which hypothesis holds, the exact
resident source (param name+fields OR static addr+layout), and a recipe to populate the converter array (or
compute the affine) **map-closed**. Then the implementation drops the prime, calls projection without the VM,
and the vmap can own the surface + input with the native map closed. (`legacy_fold` already covers the fold;
this brief covers only the base affine's source.)

## Pointers
- Findings this builds on: `windows_world_to_mapspace_projection_re_findings.md`,
  `world_map_projection_re_findings.md`, `windows_legacyconv_param_live_re_findings.md`.
- Code: `src/goblin_worldmap_probe.cpp` (project_live / project_page, VM resolve),
  `src/worldmap/legacy_fold.cpp` (the resident-param fold precedent — this is the pattern to mirror),
  `src/worldmap/map_renderer.cpp` (`project_marker` + the baked-affine fallback).
- Signatures: `goblin::sig::WORLDMAP_PROJ_LOOP` / `WORLDMAP_PROJ_POINT` (re_signatures.hpp). Add a
  `WORLDMAP_VM_CTOR` (FUN_1408855b0) / `WORLDMAP_CONV_BUILDER` (FUN_140876100) AOB when found.
- Strategy hook: `docs/plans/imgui_only_map_plan.md` (this replaces the M5 native-draw-cut path with a
  "close the menu, project off-VM, own the input" architecture — see the session note there).
