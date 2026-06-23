# Findings — runtime raw-MSB residency (depend on the REAL files, no bake)

Goal (quentin): one DLL that derives loot from the **actually-loaded mod's files** (ERR/ERTE/Convergence/
Vanilla) instead of a committed `items_database.json`. Investigated live (Altus Plateau) 2026-06-24 with
the reusable tools (`tools/ghidra/rtti_index.txt` + `query.java`) + RPM probes.

## The map-streaming + file-load code path (RE'd)
```
CSFileStep (FD4 step, ~er+0x1f71b0)            per-frame streaming step (the "hot loop" disk reads)
 → CSFileRepository (er+0x1f5fd0)              VFS: logical path -> mounted device
   → CSEblFileManagerImpl / BhdMultiMountFileCap   EBL mounts: vanilla .bhd/.bdt + ModEngine OVERRIDES
     → DLIO file devices/operators            DLFileOperator (open/read/seek/getSize/close),
        DLEncryptedBinderLightFileDevice(DLEBL)=decrypt, DCXFileInterpreter/OodleDecompressionStream=DCX
        low-level Win32: CreateFileW @ FUN_141fc13f0 ; ReadFile @ FUN_1419d3780
 → CSMapbndFileCap (er+0x2098e0, keyed "map:/m60.." @+0x18)   the loaded .mapbnd file
   → CSMapbndResCap (FD4ResRep, keyed "m60.." @+0x18)         the PARSED resource (267-268 resident)
      → CSMsbPartsGeom 8001 / CSMsbPartsMap 343 (parsed parts, positions) ; CSMsbEvent=1 (events discarded)
```
The mount is resolved by the game, so **whatever the active mod provides is what's read** — no per-mod
logic needed in the DLL.

## ★ KEY FINDING — the raw decompressed MSB is RESIDENT and parseable
Live magic scan (committed-private):
- **`MSB ` : 25 hits** — full decompressed MSB files resident (the currently-streamed maps). Each verified
  real: `MSB ` header + **~2000 `AEG…` part names (utf-16)** in its 3 MB window.
- `BND4`:13, `BHF4`:27, `DCX\0`:2 (binders / a couple still-compressed).
- `CSMapbndFileCap`: 267 (keyed `"map:/m60_xx_xx_xx"`), `CSMapbndResCap`: 268 (keyed `"m60_xx_xx_xx"`).

Since a resident MSB blob = the raw file bytes, it contains BOTH the **Parts** (name + transform/position)
AND the **Events.Treasure** (part ref + `itemLotId`) — the exact data the offline bake parses. The game
discards the parsed *event objects* (`CSMsbEvent`=1) but the **source MSB bytes stay resident**.

## Verdict — route B (parse resident MSBs) is the clean answer; A not needed
- **A (drive the game's file loader to read files by path):** feasible — the infra is fully present and
  mount-aware (`DLFileOperator` + `DLEBL` + `CSEblFileManagerImpl`) — but it needs RE of the operator/
  device-manager lifecycle **+ DCX/Oodle decompression** + an MSB parser. Heaviest route.
- **B (parse the resident decompressed MSBs):** needs ONLY a C++ MSB parser — no file-I/O RE, no DCX
  (the game already decrypted+decompressed), no committed bake. Auto-adapts to any mod. **Recommended.**
- **C (bundle the existing C# extractor, run first-launch on the user's mounted files):** lowest new-code,
  reuses the proven parser; good fallback for full-upfront coverage.

## Architecture (one DLL, no per-mod heuristic, no committed bake)
- **Loot content** (`lotId → items`): read live `ItemLotParam` (already done, `resolve_loot_item_textid`).
  Reflects the active regulation automatically.
- **Placements** (`part → lotId`, `part → position`): **parse the resident MSBs (route B)** → join with
  the live ItemLotParam → loot identity + position, from the REAL active-mod files. Persist a per-user
  cache, filled incrementally as maps stream in.
- Net: identical result for ERR/ERTE/Convergence/Vanilla with ZERO per-mod config — the DLL reads
  whatever the game mounted.

## Limitation + the only reason to also do A/C
Only the ~25 currently-streamed maps are resident at once → **B gives incremental coverage** (per-user
cache fills as the player explores; fresh areas populate the moment they stream in). For a **complete
world map upfront without exploring**, read the non-loaded map files too — via the game loader (A) or a
bundled first-launch extractor (C). For an explore-as-you-go overlay, B alone suffices.

## Next step
Write a minimal **C++ MSB (MSBE) parser**: header → Parts table (name + transform) + Events table
(Treasure subtype: part index/name + `itemLotId`). Source the MSB bytes from the resident blobs
(via `CSMapbndFileCap`/`CSMapbndResCap` keyed by map name, or a bounded `"MSB "` scan). Format reference:
SoulsFormats `MSBE`. Then: parse → join live `ItemLotParam` → per-user loot cache → overlay.

Tooling: `D:\ghidra_scripts\msb_event_scan.py`, `mapbnd_buf_check.py`, `raw_msb_check.py`,
`msb_content_check.py`, `find_io.java`, `tools/ghidra/{rtti_index.txt,query.java}`.
