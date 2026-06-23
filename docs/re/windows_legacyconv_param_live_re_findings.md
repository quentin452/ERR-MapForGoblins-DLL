# RE findings — read WorldMapLegacyConvParam live + replicate the fold (kill baked LEGACY_CONV)

Answers `docs/re/windows_legacyconv_param_live_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v64`,`re_v74`) + the shipped paramdef
(`tools/paramdefs/WorldMapLegacyConvParam.xml`) + the mod's existing baker
(`tools/generate_data.py::generate_legacy_conv_cpp`). App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only. Extends `windows_world_to_mapspace_projection_re_findings.md`
§2 (the fold fn `FUN_1408775e0`).

---

## 0. TL;DR

The fold is fully recoverable from the live regulation param — **the live-param fetch already exists
in the mod, and the compose math already exists in the Python baker.** Porting it to a map-closed C++
fold is mostly wiring, with two correctness upgrades over today's baker.

- **The param is MULTI-HOP (chained).** A legacy block's `dst` can be another non-overworld block
  (e.g. Ashen Capital m35 → Leyndell 11 → overworld 60). The **engine flattens this at VM-build**
  (`FUN_1408776e0` composes + inserts into the VM tree), so its runtime fold `FUN_1408775e0` is a
  single lookup. **Map-closed we have no VM tree → we must compose the chain ourselves** (exactly
  what `generate_legacy_conv_cpp::resolve()` already does in Python).
- **Identity / terminal test = `areaNo ∈ [50, 88]`** (`FUN_140660fe0`: `area − 0x32 < 0x27`), NOT
  just `{60,61}`. Areas 10–43 (legacy + base-UG 12) are sources → fold; 50–88 (overworld/field) pass
  through. The current baker stops at `{60,61}` — it would drop any chain terminating at a 50–88 area
  that isn't 60/61. Use the range.
- **Key on the FULL block `(area,gx,gz)`** (the engine's tree key is the packed
  `area<<24|gridX<<16|gridZ<<8`). Today's baker keys on `(area,gx)` only → the area-16 "wrong region"
  bug. Use the full block.
- **The fold is a world-space translation:** `dstWorld = srcMarkerWorld + (dstBaseWorld −
  srcBaseWorld)` of the matched row, `dstArea = row.dstAreaNo`, composed up the chain.
- **Live fetch is one line** (existing infra): `from::params::get_param<…>(L"WorldMapLegacyConvParam")`.
  Params are resident **map-closed** (regulation-loaded), so the minimap can fold without the VM.

---

## 1. The paramdef — `WORLD_MAP_LEGACY_CONV_PARAM_ST` (row = 0x30 bytes)

From `tools/paramdefs/WorldMapLegacyConvParam.xml`:
```
+0x00  u8   disableParam_NT:1 + reserve:7
+0x01  u8[3] reserve2
+0x04  u8   srcAreaNo            ← SRC block key
+0x05  u8   srcGridXNo
+0x06  u8   srcGridZNo
+0x07  u8   pad1
+0x08  f32  srcPosX              ← SRC base point (world ref of this row)
+0x0C  f32  srcPosY
+0x10  f32  srcPosZ
+0x14  u8   dstAreaNo            ← DST block
+0x15  u8   dstGridXNo
+0x16  u8   dstGridZNo
+0x17  u8   pad2
+0x18  f32  dstPosX              ← DST base point
+0x1C  f32  dstPosY
+0x20  f32  dstPosZ
+0x24  u8   isBasePoint:1        ← each srcMapId has exactly one base point
+0x25  u8[11] pad
```
World of a (grid,pos): `world = gridNo·256 + pos`. The row's translation delta =
`dstBaseWorld − srcBaseWorld` per axis.

This matches what the engine's prebuilt tree node stores (`FUN_1408775e0`/`FUN_1408763b0`): node
`+0x1c` = packed src key, `+0x20` = packed dst id, `+0x24/+0x28/+0x2c` = the **precomputed world
delta** (f32 X/Y/Z) — i.e. the engine bakes `dstBaseWorld − srcBaseWorld` into the tree at build.
We compute that delta from the raw param fields instead.

## 2. Live fetch (deliverable #2) — already in the mod

`goblin_inject.cpp` already does `from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>
(L"WorldMapPointParam")` and walks `SoloParamRepository` via `find_param_res_cap_by_name`
(AOB `SOLO_PARAM_LIST`, `re_signatures.hpp`). The legacy-conv param is the identical path:
```cpp
// add the struct (layout from §1) to from::paramdef, then:
auto conv = from::params::get_param<from::paramdef::WORLD_MAP_LEGACY_CONV_PARAM_ST>(
                L"WorldMapLegacyConvParam");   // iterable {row_id, row_ptr}
```
Params are loaded with the regulation and stay resident **whether or not the world map is open** →
this is the map-closed source the minimap needs. Build the lookup once (lazy), rebuild on regulation
reload (same trigger the mod already uses for live graces).

## 3. The fold (deliverable #3) — exact, mirrors `FUN_1408775e0` + the chain compose

```c
// identity / terminal: FUN_140660fe0(packedId) = (areaByte - 0x32) < 0x27  →  area in [50,88]
inline bool conv_terminal(u8 area){ return (u8)(area - 0x32) < 0x27; }   // 0x32=50, +0x27=39

// one row's world delta (computed from the raw param, = the engine's node +0x24/+0x2c)
//   deltaX = (dstGX*256+dstPosX) - (srcGX*256+srcPosX)
//   deltaZ = (dstGZ*256+dstPosZ) - (srcGZ*256+srcPosZ)

// fold(area,gx,gz, posX,posZ) -> (area,gx,gz, posX,posZ) in the overworld/field frame
Folded fold(u8 area,u8 gx,u8 gz, float posX,float posZ) {
    double wx = gx*256.0 + posX, wz = gz*256.0 + posZ;
    int guard = 0;
    while (!conv_terminal(area) && guard++ < 8) {            // multi-hop, cycle-bounded
        const Row* r = lookup(area, gx, gz);                 // FULL block key (area,gx,gz)
        if (!r) break;                                       // no row → leave as-is (gate later)
        wx += (r->dstGX*256.0 + r->dstPosX) - (r->srcGX*256.0 + r->srcPosX);
        wz += (r->dstGZ*256.0 + r->dstPosZ) - (r->srcGZ*256.0 + r->srcPosZ);
        area = r->dstAreaNo;                                 // next hop's block
        gx = (u8)(wx/256.0); gz = (u8)(wz/256.0);            // renormalize (FUN_140877840)
    }
    int fgx = (int)floor(wx/256.0), fgz = (int)floor(wz/256.0);
    return { area, (u8)fgx, (u8)fgz, (float)(wx-fgx*256.0), (float)(wz-fgz*256.0) };
}
```
- **`conv_terminal` (= `FUN_140660fe0`)**: `area ∈ [50,88]` → don't fold (overworld 60/61 + DLC/field).
  Sources are areas < 50 (legacy 10–43, base-UG 12) → folded. **This replaces the baker's `dst ∈
  {60,61}` terminal**, which silently drops chains ending at a 50–88 area ≠ 60/61.
- **`lookup(area,gx,gz)`** keys on the **full packed block** `area<<24|gridX<<16|gridZ<<8` (engine
  tree key, low/lod byte 0). Today's baker keys on `(area,gx)` → the area-16 bug; fix it here.
- **Translation preserves the marker's offset from the row's base point** (the row is a *base point*
  per `isBasePoint`; the marker need not sit on it). This is the projection-doc Step-A translation.
- `FUN_140877840` (renormalize) is implicit: we carry world fully and re-split grid/pos at the end;
  no separate step needed.

After folding to the overworld/field frame, apply the known per-page affine (converter dump:
`origin = gridbase·256`, `bias 128`, `scale 1`, Z-flip) — same as the map-open path, just sourced
from the param instead of the VM.

## 4. Single- vs multi-hop (deliverable) — **MULTI-HOP, compose required**

The raw param chains (m35→11→60; the baker comment + `resolve()` recursion prove it). The engine
hides this by **flattening at VM build** (`FUN_1408776e0` does a second tree walk to fold the dst,
then `FUN_1408763b0`/`FUN_1408767c0` insert the composed node — so the runtime tree maps each src
block *directly* to its terminal). A map-closed replication has no flattened tree, so it must
compose the chain at fold time (or pre-flatten once on load — recommended, mirroring the engine /
the existing Python baker). The mod's `generate_legacy_conv_cpp::resolve()` is the working reference;
the live C++ version is that algorithm with the two §3 corrections.

## 5. Worked example (validate against the live VM / baked table)

Area-16 (Raya Lucaria interior), block `(16, 0, 0)`, baked row:
`{16,0,0, srcPos(-10.16, 15.894)} → {60,36,53, dstPos(1.742, 125.753)}` (single hop, dst 60 ∈ [50,88]).
- `srcBaseWorld = (0·256−10.16, 0·256+15.894) = (−10.16, 15.894)`
- `dstBaseWorld = (36·256+1.742, 53·256+125.753) = (9217.742, 13693.753)`
- `delta = (9227.902, 13677.859)`
- a marker at `(16,0,0, posX=20, posZ=−5)` → `srcWorld=(20,−5)` → `dstWorld=(9247.902, 13672.859)`
  → `(area 60, gx 36, gz 53, posX 31.902, posZ 120.859)`.
Cross-check: this must equal what the live VM `FUN_1408877d0` produces for the same marker, and what
the map-open overlay already draws. For a **chained** check use an m35 (Ashen Capital) block → expect
two hops down to 60.

## 6. Plan / what changes in the mod

- Add `WORLD_MAP_LEGACY_CONV_PARAM_ST` (struct §1) to `from::paramdef`; fetch live (§2).
- Replace `project_dungeon_row_to_overworld` + the baked `LEGACY_CONV` (`generated*/goblin_legacy_conv.hpp`)
  with the §3 live fold (full-block key, `[50,88]` terminal, compose). Pre-flatten on load.
- The minimap / `marker_world_pos` then project **map-closed** from live param data; delete
  `LEGACY_CONV` (all `generated*` variants) and `dlc_ug_eyeball`. Keep the VM `worldmap_probe::project`
  as the map-open fast path (already validated); this is the map-closed equivalent from the same
  regulation data → no drift, no bake.

## 7. Handles

- fold `FUN_1408775e0` `0x8775e0`; identity `FUN_140660fe0` `0x660fe0` (`area∈[50,88]`); renormalize
  `FUN_140877840` `0x877840`; build-time flattener `FUN_1408776e0` `0x8776e0` (insert
  `FUN_1408763b0`/`FUN_1408767c0`). param fetch = existing `get_param` / `SOLO_PARAM_LIST` AOB.
- tree node: key `+0x1c` (packed src), value `+0x20` (packed dst), `+0x24/+0x28/+0x2c` (f32 world
  delta). paramdef offsets §1. Resolve fns by AOB; the param + the math are the stable contract.
- Runtime confirm (game not running during this RE): dump a few live `WorldMapLegacyConvParam` rows
  map-CLOSED (confirm resident), fold a known m16 + a chained m35 marker, compare to the map-open VM
  result for the same marker.
```
