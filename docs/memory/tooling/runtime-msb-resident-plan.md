---
name: runtime-msb-resident-plan
description: "Plan to make MapForGoblins depend on the live mod's REAL files (no committed bake) by parsing resident MSBs"
metadata: 
  node_type: memory
  type: project
---

<user>'s goal: **one DLL that derives loot from the actually-loaded mod's files (ERR/ERTE/Convergence/
Vanilla), not a committed `items_database.json`** — no per-mod bake, no per-mod heuristic. Investigated
2026-06-24 (live, Altus), commit dda54a2, doc `docs/re/windows_runtime_msb_resident_re_findings.md`.
See [[ghidra-re-tooling]], [[fieldins-pool-registry-re]].

**KEY FINDING: the raw decompressed MSB is RESIDENT** — live `MSB ` magic scan = **25 full MSB files**
in memory (the streamed maps), each real (`MSB ` header + ~2000 `AEG` part names utf-16). The raw map
bytes contain Parts (name+transform) AND Events.Treasure (part+`itemLotId`) — the game discards parsed
event objects (`CSMsbEvent`=1) but the source MSB stays resident. So we can re-parse it ourselves.

**Map-streaming/file-load path RE'd:** CSFileStep (~er+0x1f71b0, per-frame) → CSFileRepository
(er+0x1f5fd0) → CSEblFileManagerImpl / BhdMultiMountFileCap (vanilla + **ModEngine overrides**) → DLIO
operators (DLFileOperator open/read; DLEBL decrypt; DCXFileInterpreter/OodleDecompressionStream) →
CSMapbndFileCap (er+0x2098e0, keyed "map:/m60.." @+0x18) → CSMapbndResCap (FD4ResRep, keyed "m60.." @+0x18,
268 resident). Win32: CreateFileW @ FUN_141fc13f0, ReadFile @ FUN_1419d3780.

**ARCHITECTURE (chosen): one DLL, no bake, no per-mod logic.**
- Loot content (`lotId→items`): live `ItemLotParam` (already done, `resolve_loot_item_textid`) — reflects
  the active regulation automatically.
- Placements (`part→lotId`, `part→position`): **route B = parse the RESIDENT MSBs** → join live ItemLotParam
  → per-user cache, filled incrementally as maps stream. **Only new code = a C++ MSBE parser** (Parts +
  Events.Treasure). No file-I/O RE, no DCX (game already decompressed), auto-adapts to any mod.
- Route A (drive the game's file loader by path) = feasible but heavier (operator/device RE + DCX) →
  not needed. Route C (bundle the C# extractor, first-launch on user's files) = fallback for full-upfront.
- Limitation of B: only ~25 streamed maps resident at once → incremental coverage (fine for explore-overlay;
  for whole-map-without-exploring add A or C).

**★ LAYOUT MAPPED + PROVEN LIVE (2026-06-24, commit cf57e61, doc `windows_resident_msbe_layout_re_findings.md`).**
Live-parsed a resident MSB (Altus m395100); full chain on real ERR data: lot=1039510400 part='AEG099_090_9000'
pos=(90.6,770.1,82.9). **This is PRE-OPEN** (the MSB Treasure placement, resident when the tile streams) —
resolves the original chest-lotId quest that §8 declared dead (§8 looked at runtime gimmicks, not the source MSB).
- **Oodle is reusable:** mod already has `OodleLZ_Decompress` callable (`g_oodle_orig` in goblin_inject.cpp, or
  GetProcAddress on oo2core_6_win64.dll) → DCX ≈ free → route A-disk is light too.
- **★ resident MSB offsets are RELOCATED to ABSOLUTE VAs** (blobBase+fileOff) at load, not file-relative.
- **Exact layout (= C++ parser spec):** header "MSB "+int1+int0x10; PARAM hdr: +0x04 offsetCount(=entries+1),
  +0x08 nameOffset(abs), +0x10 entryOffsets[abs], then nextParam. 6 sections MODEL/EVENT/POINT/ROUTE/LAYER/PARTS.
  EVENT entry: +0x0c eventType (**4=Treasure**), +0x20 typeDataOffset(abs). Treasure typedata: **+0x08 partIndex**,
  **+0x10 itemLotId**. PARTS entry: +0x00 nameOffset(abs), **+0x20 Vector3 position**. m80 overworld tiles=0 events;
  detail/legacy maps carry treasure.

**★ DISK route ALSO validated (commit 822fcd7): decoded Stormveil m10_00 from `<windows_downloads>\ERR_mod\map\MapStudio`
while player in Altus (not loaded) → 113 treasures, EXACT match to items_database.json** (lot=10000850
part=AEG099_990_9002 pos=(-298.4,64.3,426.3)). So full-upfront from disk works too. Two parser-critical rules:
- **ERR loose `.msb.dcx` = DCX_DFLT (zlib), NOT Oodle** → C++ uses zlib/miniz; Oodle (already callable) only for
  any DCX_KRAK. DCX: DCS\0→BE uncompSize@+4/compSize@+8; data @ find("DCA\0")+8 (78 da zlib).
- **Offset-base differs:** PARAM-level offsets (section name, entry-offset array) = FILE-ABSOLUTE in both.
  ENTRY-INTERNAL offsets (name/entity/typeData) = **ENTRY-RELATIVE on disk** (td=entryStart+read(entry+0x20)),
  but RELOCATED to ABSOLUTE VAs in the resident RAM copy (read directly). Parser needs a disk/resident flag.
  Inline data (PARTS pos vec3 @+0x20) = same in both. partIndex==0xFFFFFFFF = item-glow/no-part (skip/region).

**★ TRANSFORM LINK VALIDATED (2026-06-24, commit 1145ff6, doc `windows_msbe_position_transform_validation.md`).**
Diffed ALL ERR `.msb.dcx` Treasure positions vs items_database.json: bake stores RAW MSB block-local (Part+0x20);
extract_markers applies `world=gridXNo·256+pos` (+legacy-conv) DOWNSTREAM → **runtime reuses the existing transform,
no new RE.** LOD0 (`_00`) maps only: MATCH=3374 (99.3%), MISMATCH=24 (NOT parser bugs — lots shared across parts/maps).
WIRING RULES: parse `_00` maps only (skip `_01/_02/_99` connect/LOD — they proxy detail parts at 128/256 offset);
pos=Part+0x20; grid from filename (=part gridXNo for detail tiles); **a lotId → MULTIPLE parts, emit each**;
partIndex 0xFFFFFFFF=item-glow no-pos. 373 MSB lots absent from bake = potential ERR loot the bake missed.
Parser already built (commit 381c88f, src/worldmap/msbe_parser); offline diff probes `<ghidra_scripts>\msbe_diff{all,00}.py`.

**NEXT STEP:** write ONE C++ MSBE parser (disk/resident offset-base flag) → enumerate resident MSBs ("MSB " scan
or CSMapbndFileCap/ResCap by name) for loaded + read on-disk .msb.dcx (zlib) for the rest → Treasure → join live
ItemLotParam → per-user cache. Validated decoder: `<ghidra_scripts>\decode_disk_msb.py`; resident probes
`live_msb_parse{,2,3,4}.py`. Both routes proven against the bake.
