# FINDINGS — disk water source, Probe 1 (MSB water-plane) → NEGATIVE; Source A (collision material) is the path

Executes Probe 1 of `far_water_surface_disk_re_prompt.md`. Offline MSB inspection via SoulsFormats
(`SoulsFormats.MSBE` through pythonnet/coreclr, `tools/lib/Andre.SoulsFormats.dll`) on the **vanilla
UXM-unpacked** install (`D:\SteamLibrary\...\ELDEN RING\Game\map\mapstudio`, base geometry = mod-agnostic),
2026-07-06. Probes: `tools/_probe_water_msb.py`, `tools/_probe_water_deep.py`.

**Verdict: Source B (identify a water plane in the MSB and read its Y) is a DEAD END.** ER MSBs carry no
water-identifiable entity, no per-part water Y, and the overworld tiles (where the big water bodies are) have
no MSB collision at all. The disk water source must come from the FLVER/collision geometry layer →
**Source A: the `hkxpwv` collision `Water`/`Swamp` material, riding the far-terrain bake.** Evidence below.

---

## 1. No water-identifiable MSB entity (19-map sample: legacy + underground rivers + overworld)
Scanned Parts / Models / Regions for water tokens (`water/umi/mizu/kawa/gawa/numa/taki/lake/sea/pond/wet/
swamp/moat/river`) across m10–m19 legacy, m12 underground (Siofra/Ainsel **rivers**), and 20 spread overworld
tiles:
- **0 water-token models, 0 water-token parts, 0 water-token regions.**
- **Part subtypes:** `Asset, Enemy, DummyEnemy, MapPiece, Collision, DummyAsset, ConnectCollision, Player` —
  **no `Water` part subtype.**
- **Region subtypes** (34 kinds: PatrolRoute, EnvironmentMapEffectBox, SFX, MufflingBox, WeatherOverride, …) —
  **no water/hazard-water region.**
- Model names are **opaque numeric codes** (`mNNNNNN` map-pieces, `hNNNNNN` collision, `AEG…` assets), NOT
  romaji/English words — so nominal token-matching can't identify a water map-piece even if one exists.

Even a literal river (Siofra `m12_01`) has **no** water-named anything. ER does not name water in the MSB
(consistent with the exe having no water strings, `windows_water_level_source_re_findings.md`).

## 2. The surface Y is NOT in the MSB (it's in the FLVER/hkx)
Deep dump of `m14_00_00_00` (Academy, sits **in** Liurnia lake) + `m12_01_00_00` (Siofra): **every MapPiece
and every Collision part has Position `(0,0,0)`, identity rotation, unit scale.** In ER the terrain FLVER (and
the collision hkx) carry world-positioned geometry themselves; the MSB part is a placement stub at the origin.
So there is **no per-part water surface Y to read** — the Y lives in the FLVER vertices / hkx mesh. This kills
Source B as a *cheap* lookup: identifying a water plane (even if possible) would still require parsing the FLVER
to get its Y — the same FLVER frontier as the far terrain.

## 3. Collision `HitFilterID` is not a water discriminator (and overworld has no MSB collision)
MSBE `Collision` parts DO carry a behaviour class **`HitFilterID`** (+ `DisableTorrent`, `PlayRegionID`,
`LocationTextID`, …). Tested whether a value means "water" — histogram across water vs dry maps:
```
m14 (Academy, in-lake):  8×73, 17×7, 11×2, 29×2, 13/15/16/22 ×1
m12_01 (Siofra river):   8×60, 11×5, 16×2, 13/15/17 ×1
m10 (Stormveil, dry):    8×98, 17×4, 9/11/13/15/19/20/23 ×1, 16×2
m60_10_09_02 (overworld):  0 collisions
m60_11_09_02 (overworld):  0 collisions
```
`8` (standard walkable) dominates everywhere; the small-count special values (11/13/15/16/17/…) appear in BOTH
water and dry maps with no water-distinct value. **`HitFilterID` does not tag water.** And the sampled
**overworld LOD tiles have ZERO MSB Collision parts** — overworld collision (where the ocean/big lakes are)
lives in the separate `hkxpwv`, not the MSB. So the MSB collision layer is useless for overworld water too.

## 4. Conclusion — Source A (hkxpwv collision material) is the only disk water source
The MSB carries no water name, no water part/region type, no per-part water Y, no water `HitFilterID`, and no
overworld collision at all. The **only** disk signal that distinguishes water is the collision **material**
(`Water`/`Swamp`) in the `hkxpwv`/`hknpMaterialLibrary` layer — exactly the runtime material-tag
(`windows_water_level_source_re_findings.md` §10), read offline. That layer is the far-terrain collision bake
(`far_terrain_heightmap_re_findings.md` §5b), so **water rides the terrain bake**: rasterize seabed Y (relief)
+ per-triangle material (Water/Swamp → the sea mask) in one pass. There is no lighter MSB shortcut.

## 5. Next — Probe 2 (the material path, unchanged from the prompt)
1. **Material ids:** in Ghidra resolve the `Water`/`Swamp` ordinals (the `MatRatio` material enum
   er+0x2bc32b8, and the `hknpMaterialLibrary` er+0x2ee36b0 material→id map).
2. **Offline `hkxpwv` decode:** one Liurnia/Siofra collision → material histogram → confirm the lakebed/river
   floor is Water/Swamp-tagged map-wide (not just wadeable edges — the open risk). This is the real work
   (Havok tagfile + `hknpCompressedMeshShape` decode, SoulsFormats/HKLib-solved offline, a build step).
3. Then feed the relief sea-tag map-wide from the bake, retire `kSeaLevelY`.

## 6. Pointers
- Prompt (sources ranked): `far_water_surface_disk_re_prompt.md`.
- Runtime side + material tag: `windows_water_level_source_re_findings.md` (§9–§11).
- The shared bake + hkxpwv/material anchors: `far_terrain_heightmap_re_findings.md` (§5b).
- Probes: `tools/_probe_water_msb.py` (taxonomy + token scan), `tools/_probe_water_deep.py` (per-map deep dump
  + HitFilterID histogram). Read the vanilla `game_dir/map/mapstudio` via SoulsFormats.

## 7. Probe 2 — offline feasibility on the LINUX box (2026-07-06, packed install)
Scoped whether the Source-A material bake can be built OFFLINE on the daily Linux box (no Windows).
Reflection-probed `tools/lib/Andre.SoulsFormats.dll` with a `dotnet run` net10 console (no pythonnet).

- **.NET runs on Linux** (dotnet SDK 10 + runtimes 8/10 present). The "pipeline is Windows-only"
  ([[mapforgoblins-pipeline-setup]]) note is about the DATA, not the runtime: that box is UXM-**unpacked**
  (`map/mapstudio/*.msb.dcx` loose). **This box is PACKED** — `GAME_DIR` has only `Data0-3.bhd/.bdt` +
  `DLC.bhd/.bdt`, no loose `map/`. So the water probes ran on Windows because the loose files were there,
  not because pythonnet needs Windows.
- **SoulsFormats decodes ER collision GEOMETRY offline.** `SoulsFormats.HKX.HKNPCompressedMeshShapeData`
  exposes `primitives` (`MeshPrimitive{Idx0..3}`), `packedVertices`/`sharedVertices`/`sharedVerticesIndex`,
  `sections`, `BoundingBoxMin/Max`. ⇒ **whole-map seabed Y / far-relief IS extractable on Linux** — the same
  bake far-terrain wants (`far_terrain_heightmap_re_findings.md`). Confirmed the types load + members present.
- **Per-triangle MATERIAL is the wall.** ER wraps the mesh in `SoulsFormats.HKX.FSNPCustomParamCompressedMeshShape`
  (`MeshShapeData` + `CustomParam` `HKXGlobalReference`, plus opaque `Unk68 / Unk80 / UnkA8` `HKArray`s).
  SoulsFormats does **NOT** name/decode the per-primitive material — it stops at the geometry. The Water/Swamp
  tag rides `CustomParam` (the `fsnpCustomMeshParameter` / `hknpMaterialLibrary` id per primitive), which this
  DLL leaves as un-typed `Unk*` buffers. So geometry is free offline; the MASK is NOT, out of the box.
- **dvdbnd read still needs our RSA.** `BHD5.Read(Memory<byte>, Game)` expects an ALREADY-DECRYPTED header —
  SoulsFormats does no RSA. To read a collision file offline: RSA-decrypt the `Data*.bhd` ourselves (keys+algo
  already in `src/worldmap/dvdbnd_reader.cpp` + [[dvdbnd-packed-reader]]), `BHD5.Read` the plaintext, prime-0x85
  hash the vpath, slice the `hkxbhd`+`hkxbdt` out of the `.bdt`, `BXF4.Read` → `h######.hkx.dcx` →
  `DCX.Decompress` → `HKX.Read`. All pieces exist; it's glue (a C# build step). **DLC maps (m40-43, DLC-OW
  tiles) need the DLC RSA key**, still not captured here (base-game water — Liurnia/Siofra — is in `Data*`,
  keys present, so the first validation is unblocked).

**Net:** offline whole-map RELIEF (seabed Y) is a green light on Linux via SoulsFormats. The water MASK is
blocked on getting the per-triangle material out of `FSNPCustomParamCompressedMeshShape.CustomParam`, which
needs ONE of: (a) hand-decode the `Unk68/Unk80/UnkA8` layout from a real extracted file, (b) add **HKLib**
(a fuller community Havok lib that models `fsnpCustomMeshParameter`), or (c) resolve the Water/Swamp ids in
Windows/Ghidra (`MatRatio` enum er+0x2bc32b8, `hknpMaterialLibrary` er+0x2ee36b0) and match them against a
hand-decoded index. Reflection-probe scratch: `$CLAUDE_JOB_DIR/tmp/hkxprobe` (throwaway).

## 8. Probe 2 — offline dvdbnd→collision chain BUILT + VALIDATED on Linux; Oodle is the one wall
Built `tools/collision_offline/` (C# net10, `dotnet run`, refs `tools/lib/Andre.SoulsFormats.dll`, our own
RSA). Validated 2026-07-06 on the packed Linux box. The chain, and exactly where it stops:
```
Data*.bhd  --RSA(c^e mod n, drop leading byte)-->  BHD5 plaintext   ✓ our BigInteger modexp
           --SoulsFormats.BHD5.Read(EldenRing)-->  hash→FileHeader   ✓ counts 5824/39684 == memory
vpath      --prime-0x85 64-bit hash--> FileHeader --.bdt slice-->     ✓ Data0 known file 21056B/DCX exact
map/mMM/mMM_XX_YY_ZZ/hMM_XX_YY_ZZ.hkx{bhd,bdt}  --> BHF4/BDF4 (uncompressed)  ✓
           --SoulsFormats.BXF4.Read-->  118 inner h*_######.hkx.dcx  ✓ (m10 hi-collision)
inner hkx.dcx  == DCX-**KRAK (Oodle)**                                ✗ WALL
           --SoulsFormats.HKX.Read--> HKNPCompressedMeshShapeData geom + FSNPCustomParam material  (blocked by ↑)
```
- **RSA + hash + slice VALIDATED end-to-end** against the memory ground truth (`[[dvdbnd-packed-reader]]`):
  Data0=5824 / Data2=39684 entries; `menu/hi/01_common.sblytbnd.dcx` slices to exactly 21056 B, `DCX` magic.
- **Collision vpath convention CONFIRMED live:** `map/mMM/mMM_XX_YY_ZZ/hMM_XX_YY_ZZ.hkxbhd`+`.hkxbdt`
  (h=hi, l=lo) resolve for m10/m14/m60 tiles → `BHF4`/`BDF4` BXF pair. The BXF pair is stored UNCOMPRESSED
  in the dvdbnd; only the INNER `hkx.dcx` are compressed.
- **THE WALL = Oodle.** Every inner `h*_######.hkx.dcx` is `DCX-KRAK`. `SoulsFormats.DCX.Decompress` KRAK
  P/Invokes `oo2core_6_win64.dll` (Windows PE) → native Linux `dotnet` errors `Could not find a supported
  version of oo2core`. So the geometry + material decode can't run until an Oodle route is wired. (MSBs are
  DCX-DFLT/zlib → managed, which is why the earlier MSB probes ran offline fine; collision is KRAK.)

### Oodle routes (pick one — all keep it on this Linux box)
1. **ooz native `.so`** — build the open-source Kraken decompressor (powzix/ooz) as a Linux shared lib,
   P/Invoke `Kraken_Decompress` from C#, and hand-unwrap the DCX-KRAK header (uncompressed/compressed sizes)
   instead of `DCX.Decompress`. ⇒ FULLY OFFLINE, pure build step, no game, no Windows. Cost: build+wire ooz
   (a new native dep) + verify Oodle-stream compat.
2. **RPC hybrid** — the game runs under Proton and its in-process `oo2core` already works
   (`dcx_decompress`); add a debug-RPC verb (thin wrapper over `read_game_file_decompressed`) that writes the
   DECOMPRESSED inner hkx to disk, then decode HKX/material offline in C#. ⇒ least code, reuses PROVEN Oodle,
   no new dep. Cost: needs the game booted; awkward for a full 1300-tile bake (one-at-a-time RPC).
3. **Wine C++ extractor** — a standalone Windows console (clang-cl+xwin, `[[build-toolchain-clang-xwin]]`)
   linking the existing `dvdbnd_reader` + `dcx_decompress` (loads `oo2core_6_win64.dll` fine under Wine), run
   under Proton/Wine on this box, dumps decompressed hkx to disk; C# decodes offline. ⇒ offline, no game, no
   new dep, reuses proven C++. Cost: a new C++ target + Wine invocation.

**Route 2 (RPC hybrid) CHOSEN + the bridge is BUILT/DEPLOYED (2026-07-06g).** New host helper
`worldmap::dcx_decompress_bytes` + debug-RPC verb **`dcx_file <in.dcx> <out>`** decompress a raw DCX blob
with the game's in-process Oodle under Proton (commit `a98b4c8`; both builds green; deployed to
`ERR/dll/offline/`). The offline tool gained `dump-inner` to stage a raw inner hkx. So the material spike is
now turnkey — see "Next run" below.

### Next run (turnkey — needs ER booted once)
```
# 1. stage a KNOWN-water inner collision mesh offline (m14 = Academy, sits in Liurnia lake):
cd tools/collision_offline
dotnet run -- dump-inner m14_00_00_00 _001000 /tmp/m14.hkx.dcx      # raw DCX-KRAK, 1017885 B
# 2. boot ER (Steam up) and decompress it via the in-process Oodle:
python tools/mfg.py repl --boot        # or `rpc` if already running (verify: mfg_build, status in-world)
#   > dcx_file /tmp/m14.hkx.dcx /tmp/m14.hkx      # -> "ok dcx_file in=1017885 out=… krak=1"
# 3. offline: SoulsFormats.HKX.Read(/tmp/m14.hkx) -> HKNPCompressedMeshShapeData (geometry, decode
#    already confirmed) + FSNPCustomParamCompressedMeshShape. Then REVERSE the material rider: correlate
#    .primitives / sections with the CustomParam Unk68/Unk80/UnkA8 buffers; histogram material ids over the
#    in-lake mesh vs a dry mesh (m10 Stormveil) to IDENTIFY the Water/Swamp ordinal empirically (present on
#    the lakebed, absent on dry). Cross-check the ordinal at runtime: raycast a lake vs dry ground and dump
#    the hit material (the near-field tag, windows_water_level_source_re_findings.md §10).
```
Decide the whole-map BAKE's Oodle route (1 ooz.so vs 3 Wine-C++) AFTER material decode is proven — no point
building the fast offline path if the material can't be read.

### Still open
- Which overworld tile(s) cover the Liurnia lake / open ocean (need a KNOWN-water tile for the histogram).
  `m60_42_36_00` resolves but isn't confirmed as water. Legacy `m14_00_00_00` (Academy) sits in the lake —
  a safe first water target; Siofra `m12_01_00_00` for a river.
- **DLC RSA key** still missing → DLC maps (m40-43, DLC-OW tiles) can't be read offline yet; base-game water
  (Data2, keys present) is enough for first validation.
- Tool: `tools/collision_offline/` (selftest / list-collision / dump-inner / extract). Reflection scratch retired.
- The material Unk-layout reverse is the remaining hard part — SoulsFormats leaves the ER per-triangle
  material as un-typed `Unk68/Unk80/UnkA8` on `FSNPCustomParamCompressedMeshShape`; needs a real decompressed
  file (now one `dcx_file` call away) to work out the mapping.
