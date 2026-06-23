# Findings — resident MSBE in-memory layout (the C++ parser spec)

Live parse of a resident decompressed MSB on the running game (Altus, MSB `m395100…`), 2026-06-24.
Proves route B end-to-end: **Treasure event → itemLotId + part → world position, from the active mod's
real MSB, resident, PRE-OPEN, no bake.** Probes: `D:\ghidra_scripts\live_msb_parse{,2,3,4}.py`.

## ★ Live end-to-end result (real ERR data)
```
lot=1039510400  part[361]='AEG099_090_9000'  pos=(90.59,770.09,82.90)   "世界樹の実(エストのかけら) 街道脇"
lot=1039510000  part[384]='AEG099_620_9000'  pos=(67.62,767.31,107.88)  "宝死体000 忌み子野営地"
lot=1039510010  part[381]='AEG099_610_9000'  pos=(-50.42,748.22,-46.04)  "宝死体001 大野営地 右奥"
lot=1039510020  part[385]='AEG099_630_9000'  pos=(-31.28,748.19,52.86)   "宝死体002 宝箱 大野営地 左奥"
... 6 treasures, each lotId + part name + position.
```
This is the static placement (the MSB `Events.Treasure`), resident the moment the tile streams — i.e.
the **pre-open** loot identity the runtime-gimmick chase (§8) could never find, because it was looking at
the spawned object, not the source MSB.

## ★★ Critical: resident MSB offsets are RELOCATED to ABSOLUTE VAs
The game fixes up every offset field to a pointer at load. So in the resident blob, `nameOffset` /
`entryOffset` / `typeDataOffset` are **absolute addresses = `blobBase + fileOffset`**, NOT file-relative.
A parser over the resident buffer: treat offsets as absolute (subtract `blobBase`, or deref directly). A
parser over a disk `.msb` (route A/C) must treat them as file-relative — handle both (heuristic:
`off > blobBase` ⇒ absolute).

## Exact layout (all confirmed live)
**File header** (blob start, magic `"MSB "`):
```
+0x00  char[4] "MSB "
+0x04  int     1            (version)
+0x08  int     0x10         (header size = first PARAM offset)
+0x0c  byte[4] 00 00 01 ff  (flags: [+0x0e]=1 unicode, [+0x0f]=0xff)
+0x10  first PARAM (MODEL_PARAM_ST)
```
**PARAM (section) header** — 6 in order: MODEL, EVENT, POINT, ROUTE, LAYER, PARTS:
```
+0x00  int   Version
+0x04  int   offsetCount        (= entries + 1)
+0x08  long  nameOffset(abs)    -> UTF-16 "XXX_PARAM_ST"
+0x10  long  entryOffset[entries] (abs)
+0x10+8*entries  long nextParamOffset(abs)   (last PARAM points to EOF)
```
**EVENT entry** (EVENT_PARAM_ST):
```
+0x00  long  nameOffset(abs)    -> UTF-16 event name
+0x08  int   eventID
+0x0c  int   eventType          ★ 4 = Treasure (7=ObjAct, 15/20/21/23/24 = others)
+0x10  int   localIndex
+0x18  long  entityDataOffset(abs)   (common: entityID, part/region refs)
+0x20  long  typeDataOffset(abs)     ★ type-specific block
```
**Treasure typedata** (eventType 4, at `typeDataOffset`):
```
+0x08  int   partIndex         ★ index into PARTS_PARAM_ST (the chest/corpse part)
+0x10  int   itemLotId         ★ ItemLotParam_map id
       (further int slots = extra itemLotIds / flags; 0xFFFFFFFF when unused)
```
**PARTS entry** (PARTS_PARAM_ST):
```
+0x00  long      nameOffset(abs)   -> UTF-16 part name (e.g. "AEG099_090_9000")
+0x20  Vector3   position (x,y,z)  ★ (followed by rotation @+0x2c, scale @+0x38 — to confirm if needed)
```
(Position is BLOCK-LOCAL; the world/map transform = the same `gridXZ·256 + local` already RE'd for the
overworld, [[ghidra-worldmap-re]]. The map id comes from the MSB's own map (from CSMapbndResCap/FileCap
name "m60.." @+0x18, or the m80 grid). Confirm absolute vs local per map type when wiring.)

## Section survey (25 resident MSBs, Altus session)
Detail/dungeon maps carry events (m39xxxx Altus: 31/21/18/12/11 events; legacy m04405200: 21; m44364400
camp: 6). The m80xxxxxx overworld grid-tiles have 0 events (placeholders only). So treasure lives in the
detail/legacy MSBs, which are resident when their tile is streamed.

## Net — the whole "real-files, no bake" plan is proven
- Enumerate resident MSBs (`"MSB "` scan, or via CSMapbndFileCap/ResCap keyed by name).
- Parse (above) → `Events.Treasure` → `{itemLotId, partIndex}` → `Parts[partIndex]` → `{name, position}`.
- Join `itemLotId` → items via live `ItemLotParam` (`resolve_loot_item_textid`, already in the mod).
- Result: loot identity + position from the **active mod's real MSB**, resident, pre-open, no committed
  bake, no DCX (game decompressed), no file-I/O. Auto-adapts to ERR/ERTE/Convergence/Vanilla.
- Coverage = streamed maps (incremental → per-user cache); full-upfront needs reading non-loaded `.msb.dcx`
  from disk (Oodle already callable via `g_oodle_orig`/GetProcAddress — see runtime-msb-resident plan).

**The C++ MSBE parser can now be written against these exact offsets.** Probes:
`live_msb_parse{,2,3,4}.py`.

## ★★★ DISK route validated — non-loaded map decoded from ERR's real files (2026-06-24)
Decoded Stormveil `m10_00_00_00.msb.dcx` straight from `D:\DOWNLOAD\ERR_mod\map\MapStudio\` while the
player was in Altus (m10 NOT loaded). `D:\ghidra_scripts\decode_disk_msb.py` → **113 treasures**, e.g.
`lot=10000850 part='AEG099_990_9002' pos=(-298.4,64.3,426.3)` — **exact match to `items_database.json`**
(`m10_00_00_00`, x=-298.403, y=64.315, z=426.311, partName AEG099_990_9002, itemLotId 10000850). Proves
full-upfront coverage from the active mod's on-disk files.

Two decisive findings for the C++ parser:
1. **ERR's loose `.msb.dcx` are `DCX_DFLT` (zlib/Deflate), NOT Oodle.** Header: `DCS\0`→ big-endian
   uncompressedSize@+4 / compressedSize@+8; `DCP\0 DFLT`; data starts at `find("DCA\0")+8` with a `78 da`
   zlib stream. So **zlib (miniz) decompresses ERR maps — Oodle only needed for any `DCX_KRAK` maps**
   (Oodle already callable via the mod, so both are covered).
2. **★ Offset-base differs disk vs resident:**
   - **PARAM-level** offsets (section nameOffset, the entry-offset array) are **file-absolute** in BOTH.
   - **ENTRY-internal** offsets (an entry's nameOffset, entityDataOffset, typeDataOffset) are
     **ENTRY-RELATIVE on disk** (add the entry start: `td = entryStart + read(entry+0x20)`), but the game
     **relocates them to absolute VAs in the resident RAM copy** (read directly). So the parser needs two
     modes: disk = entry-relative; resident = absolute. Inline data (e.g. PARTS position vec3 @+0x20) is
     not an offset → same in both.
3. `partIndex == 0xFFFFFFFF` for item-glow / no-physical-part treasures (drops, event items) → skip or
   fall back to a region/point for position; the chest/corpse treasures all resolve with a real part+pos.

Net: **both routes proven** — resident MSBs (loaded, offsets absolute) + on-disk `.msb.dcx` (non-loaded,
zlib + entry-relative). One C++ MSBE parser with a disk/resident offset-base flag covers the whole map
from the active mod's real files, no committed bake. Probe: `D:\ghidra_scripts\decode_disk_msb.py`, `dbg4.py`.
