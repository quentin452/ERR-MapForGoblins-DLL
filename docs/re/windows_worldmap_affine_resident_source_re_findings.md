# RE FINDINGS (Windows/Ghidra, 2026-07-06) — resident source of the world→map-space converter affine

Answers `windows_worldmap_affine_resident_source_re_prompt.md`. Static decompile of the `WorldMapViewModel`
ctor + converter builder on `D:\ghidra_proj2\ER` (app 2.6.2.0, imagebase 0x140000000), via
`query.java`/`xrefsto.java`. No live game needed.

## VERDICT — Hypothesis A: the base affine source is EXE-INVARIANT (no mod-changeable param)

Every input the converter builder stamps into a slot comes from an **exe-controlled** source — `.rdata`
constants, exe-zeroed `.data`, or code immediates — **not** from a regulation param a mod could change. Only
the legacy-dungeon fold is param-driven, and that is already resident + handled (`legacy_fold`). So the
converter affine can be rebuilt **off-VM, map-closed, mod-agnostic** — the "silent prime" is droppable.

## The mechanism (decompiled)

- **Builder `FUN_140876100`** (50 bytes) just COPIES its args into the 0x30-byte slot — it holds no
  constants itself:
  ```
  slot+0x08 = *key;  slot+0x0c/0x14 = origin{X,Z};  slot+0x18 = bias{X,Z};  slot+0x20 = scale;  slot+0x28 = legacyNode
  ```
  Sole caller = the VM ctor.
- **VM ctor `FUN_1408855b0`** prepares those args and calls the builder twice (slot@VM+0xF8 and +0x158):
  ```c
  local_408 = _DAT_142ad8390;                 // bias   -> er+0x2ad8390
  local_3f8 = DAT_144802260;                   // origin -> er+0x4802260
  uVar8     = FUN_140660d20(&.., 0x3c,0x1c,0x40); // key: area 0x3c=60 (overworld);  2nd call 0x3d=61 (DLC)
  local_4e8 = DAT_14329e678;                    // scale  -> er+0x329e678
  FUN_140876100(VM+0xf8, key, &origin, &bias);
  ```

## Source of each field (addresses + PE section, file-verified)

| Field | Address | Section | File value | Verdict |
|---|---|---|---|---|
| **bias** X,Z | `er+0x2ad8390` | `.rdata` (const) | `{128.0, 128.0}` | ✅ baked, mod-invariant |
| **scale** | `er+0x329e678` | `.rdata` (const) | `1.0` | ✅ baked, mod-invariant |
| **origin** X,Z | `er+0x4802260` | **`.data` (writable)** | file default `{7487.0, 524256.7}` (garbage) — **zeroed at init** | ✅ exe-controlled (see below), NOT param |
| **key/area/grid** | ctor immediates | `.text` | `0x3c`=60, `0x3d`=61, `0x1c`, `0x40` | ✅ baked, mod-invariant |
| legacy fold | `param_6` | regulation param | `WorldMapLegacyConvParamGroup` | already resident + `legacy_fold` handles it |

**Origin is a zeroed `.data` global, not `.rdata`.** `xrefsto 0x144802260` → **exactly ONE write** (1483
reads): `FUN_140105e00`, which is `{ DAT_144802260 = 0; _DAT_144802268 = 0; }` — it just **zeroes** the
global at init. No param/resource writer exists. So the base origin the ctor stamps is exe-controlled
(effectively 0), never mod-loaded. bias/scale are true `.rdata` constants.

## ⚠ One thing to reconcile LIVE (the empirical validation — Linux daily-build box)

The STATIC ctor stamps `origin = 0` (zeroed global), `bias = 128`, `scale = 1`. Our proven-working baked
affine is `mapX = worldX − 7040`, `mapZ = −worldZ + 16512` (i.e. origin 7168/16384 in the UNIFIED world
frame). These are NOT contradictory — the converter's input is **area-LOCAL** `{px,pz}` + a packed
`(area,gx,gz)` id (per `windows_world_to_mapspace_projection`), whereas our baked affine takes the unified
`grid*256+local` frame; the 7168/16384 offset lives in the grid decode, not the converter origin. But this
is exactly what the empirical test must confirm:

- **Build the 8-slot converter array off-VM** from these values (origin 0 / bias 128 / scale 1 / immediate
  keys + `legacy_fold` for the fold), call `FUN_140876140` per slot (`WORLDMAP_PROJ_POINT`) with the native
  map **never opened**, and compare `(u,v)` to the map-open reference (`proj 60 42 36 → u=3712 v=7296`).
  **du/dv == 0 ⇒ the VM is unnecessary**, the prime + `world_map_open()` coupling can go.
- If a residual offset appears, dump the LIVE converter slot's `+0x0c/+0x14` origin (map-open) and compare
  to the static 0 — that isolates whether map-open repopulates origin from map data (would be the only
  remaining map-open dependency) or whether the offset is purely in the grid decode (fully static).

## Validation HARNESS — IMPLEMENTED 2026-07-06 (Linux); empirical RUN still pending

The off-VM validation path is now wired end-to-end (no engine-builder needed — we populate a 0x30 converter
slot directly and run the resolved `FUN_140876140`, so the `WORLDMAP_VM_CTOR`/`WORLDMAP_CONV_BUILDER` AOBs
were NOT required):

- `worldmap_probe::project_offvm(...)` (`src/goblin_worldmap_probe.cpp`) — builds a converter slot in our
  own memory from caller-supplied fields (`legacyNode=0`, base affine only) and runs `FUN_140876140`
  map-closed. Both builds (single + hot-reload split) link green; DLL deployed.
- RPC verbs `proj_conv` (off-VM projection) + `conv_affine` (capture the live slot fields).
- `tools/rpc_tests/test_converter_offvm.py` — two checks: (1) **never-opened** projects `60/42/36` off-VM
  under BOTH candidate constructions — `unified` (origin 7168/16384, gridbase 0/0) vs `static` (origin 0/0,
  gridbase 28/64) — to settle the origin-0-vs-7168 caveat by which reproduces `u=3712 v=7296`; (2)
  **capture-replay** — open once, capture the live fields + a live `proj`, close, replay off-VM, assert
  `du/dv==0`. Registered in `.vscode/tasks.json`.

**RUN STILL PENDING (env-blocked 2026-07-06):** the daily box had a pre-existing **D-state `eldenring.exe`
husk** (RSS 0, unreapable — the documented GPU/IO wedge) so a fresh cold-boot was unsafe. Run
`python tools/rpc_tests/test_converter_offvm.py` on a clean box → the `[OFFVM]` line names the winning
construction and the `du/dv==0` check confirms droppability. Only AFTER a green run wire `project()` /
`get_converter_affine()` to the off-VM fallback + drop the prime.

## Deliverable / next

The base affine has **no mod-variable param source** → the implementation can populate the converter array
map-closed from exe-invariant values + `legacy_fold`, or just replicate the affine, and drop the silent
prime. Add `WORLDMAP_VM_CTOR` (`FUN_1408855b0`) / `WORLDMAP_CONV_BUILDER` (`FUN_140876100`) AOBs to
`re_signatures.hpp` if the off-VM rebuild path is taken. Then `imgui_only_map_plan.md`'s "close the menu +
project off-VM + own input" architecture is unblocked. Runtime validation recipe above; extend
`tools/rpc_tests/test_converter_residency.py` with a "never-opened" variant.

## Tooling note

`query.java` (decompile + entry AOB + rip-rel statics + callers) and a new `xrefsto.java` (all refs to a
data address, WRITE-flagged — the single-writer test that settled origin) live in `D:\ghidra_scripts\`.
Section/writability + const values read straight from the exe via a small PE RVA→file-offset parser.
