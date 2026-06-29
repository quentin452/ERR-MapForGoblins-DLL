---
name: msbe-parser-supersedes-bake
description: "MSBE parser (resident + disk routes) SHIPPED — now the live DiskMSB source (8381 markers); drove the ERR no-bake migration to completion (baked MAP_ENTRIES = 0); superseded §8 MapIns chase + FieldIns cache"
metadata: 
  node_type: memory
  type: project
---

**UPDATE 2026-06-27 — SHIPPED + NO-BAKE MIGRATION COMPLETE (ERR).** The parser is now the
live `DiskMSB` source and Phase 2 no-bake is DONE: ERR `src/generated/goblin_map_data.cpp` =
17-line stub, `MAP_ENTRIES` empty, **baked residual = 0, all 61 categories 🟢 off-bake**
(Older note: HEAD `4a7716d` "Merge Phase 2: no-bake migration"; import checkout 2026-06-28 is
`master@931438d`; scoreboard `docs/nobake_scoreboard.md` is the current source).
Final marker provenance: **disk 8381 / live 469 / live-cls 74 = 8850 total**. Disk pass parses
`_00` (LOD0) tiles only (651/964; `_01/_02` LOD proxies, `_10/_11/_12` mostly `_00` dupes —
skipping loses ~no unique markers). Residual recovery levers that closed the last categories:
asset-aware + loose-anchor EMEVD join, per-tile EMEVD enemy-death awards, sequence-sibling
walk (boss bundle drops), explicit bossEntity@16 (Reforged Rune Pieces), cross-tile LOD
treasures, merchant-phantom drop via live ShopLineupParam, Kindling from disk SFX regions.
NOTE: only the **ERR** variant is off-bake — `generated_vanilla/erte/convergence` still ship
full baked `MAP_ENTRIES` (legacy). See [[loot-identity-stable-err-additive]] (now off-bake)
and [[one-dll-externalize-mapdata]] (ERR no longer has a bake to externalize).

Windows RE agent SOLVED the full MSBE parser (2026-06-24, commits cf57e614 + 822fcd7c,
doc `docs/re/windows_resident_msbe_layout_re_findings.md`). This is the real jackpot — it
supersedes the whole runtime-gimmick loot chase AND the baked-data architecture.

**Chain (proven end-to-end on real ERR data, exact-match to items_database.json):**
MSB `Events.Treasure` (eventType==4) → typedata {partIndex@td+0x08, itemLotId@td+0x10} →
PARTS[partIndex] → {nameOffset@+0x00, position vec3@+0x20} → join live ItemLotParam
(`resolve_loot_item_textid`). Pre-open, on tile-stream, no spawned object needed.

**Two routes, both validated:**
- RESIDENT (loaded tiles): scan for `"MSB "` blob / via CSMapbndResCap. Offsets RELOCATED to
  ABSOLUTE VAs at load (entry-internal name/entity/typeData offsets = blobBase+fileOff, deref
  directly). Detail/legacy/dungeon MSBs carry events; m80 overworld grid-tiles have 0 events.
- DISK (non-loaded): read mod's `map/MapStudio/*.msb.dcx`. **ERR loose maps = DCX_DFLT (zlib/
  miniz), NOT Oodle** (Oodle only for any DCX_KRAK; already callable). Entry-internal offsets are
  ENTRY-RELATIVE on disk (td=entryStart+read(entry+0x20)) → parser needs disk/resident flag.
  PARAM-level offsets file-absolute in BOTH. Inline data (PARTS pos vec3) same in both.
  partIndex==0xFFFFFFFF = item-glow/no-part → skip or region-fallback.
- Position = BLOCK-LOCAL → world via gridXZ·256+local (same overworld transform already RE'd).

**SUPERSESSIONS (what this retires):**
- §8 MapIns/FieldIns runtime chase in [[loot-identity-stable-err-additive]] — DEAD. Looked at
  spawned object (1/343 loaded). MSB = source placement, full coverage, pre-open.
- The FieldIns→lot CACHE (#1) + bg-thread move (#2) — NOT NEEDED. Direct resident parse.
- MAPINS diag walker (diag_lot_memscan, src/goblin_collected.cpp) — retire once parser lands.
- Baked MAP_ENTRIES (3.37MB compiled-in) + the 4-DLL-per-mod variant problem in
  [[one-dll-externalize-mapdata]] — disk route = full upfront coverage from active mod's real
  files, no committed bake, auto-adapts ERR/ERTE/Convergence/Vanilla. Aligns with
  [[live-param-vs-baked-data]] (read real files, kill per-mod drift).

**STATUS (2026-06-24): DISK-ROUTE PARSER WRITTEN + VERIFIED OFFLINE ON LINUX.** New module
`src/worldmap/msbe_parser.{hpp,cpp}` (parse_msb disk-mode + dcx_decompress via stb's
stbi_zlib_decode_buffer — NO new dep, stb already in tree). Offline test harness
`tools/msbe_test/msbe_test.cpp` (build: clang++ -std=c++20 -Isrc -Ithird_party
tools/msbe_test/msbe_test.cpp src/worldmap/msbe_parser.cpp src/stb_image_impl.cpp). ERR mod
files are LOCAL at <ERR_ROOT>/mod/map/MapStudio/ (964 .msb.dcx) → can
verify offline, no game/Windows build. m10_00_00_00 → 113 treasures incl lot=10000850
part='AEG099_990_9002' pos=(-298.4,64.3,426.3) EXACT items_database match. Full sweep: 708
DFLT→4075 treasures 0-fail, 256 KRAK correctly flagged, 0 crash.
**DCX gotcha (cost a bug): format block "DCP\0"@0x24, 4-char fmt@0x28 (DFLT|KRAK) — NOT
0x28/0x2c.** uncompSize=DCS+4(0x1c) big-endian; zlib stream @ find("DCA\0")+8.
**KEY FINDING (corrects the RE doc): ERR ships a MIX, not all-DFLT.** ERR-MODIFIED maps =
repacked DCX_DFLT (zlib, 708 files = exactly the loot DELTAS vs vanilla); vanilla-UNTOUCHED
maps stay DCX_KRAK (Oodle, 256). So DFLT-only parse (no Oodle) = precisely ERR's added/changed
loot → a SUPPLEMENT/OVERRIDE layer over baked needs NO Oodle. Oodle (DLL-side g_oodle_orig,
callable) only for FULL no-bake coverage.
NEXT = integration: filename→mapId (m{area}_{gridX}_{gridZ}, decimal→byte pack:
mapId=(area<<24)|(gX<<16)|(gZ<<8)|sub; m60→0x3c etc), block-local pos→worldmap via existing
projection [[worldmap-projection-re-solved]] (legacy/detail maps need per-map transform, not
just grid·256), join resolve_loot_item_textid(lot,1,0), feed marker pipeline (parser output
maps 1:1 onto MapEntry: lotId+lotType=1, partName→object_name/geom_slot=InstanceID-9000/pos).
Open: map-folder discovery for the DLL (mod/map/MapStudio rel to game exe), resident route
(deferred, needs CSMapbndResCap RE to avoid a "MSB " RAM-scan anti-pattern).

**OODLE OFFLINE-VERIFIED + SCOPE PINNED (2026-06-24).** Game install =
<steam_compat_root>/steamapps/common/ELDEN RING/Game/ (eldenring.exe +
oo2core_6_win64.dll). KRAK route tested offline via clang-cl+xwin Windows exe under
wine-11.10 loading the REAL oo2core (tools/msbe_test/msbe_oodle_test.cpp; build: clang-cl
/c objs then lld-link directly — clang-cl driver can't spawn lld-link itself; flags = the
build-linux xwin /imsvc dirs + /libpath crt/ucrt/um + kernel32.lib). OodleLZ_Decompress
(14-arg, threadPhase=3) WORKS: e.g. m45_01 17604→840384 bytes, parse ok.
**In ERR's mod folder: all 256 KRAK maps = 0 treasures; all 4075 treasures in the 708 DFLT maps**
(ERR re-saved its treasure maps as DFLT, left treasureless ones KRAK). ⚠️ CORRECTION (Windows agent
fb2e289 + 4-profile runtime): this was ERR-FOLDER-SCOPED — for VANILLA/non-ERR profiles, treasure maps
stay DCX_KRAK → Oodle IS NEEDED in general (Vanilla profile = 949 maps/3062 lot rows replaced, all KRAK).
So "loot needs no Oodle" (my commit 25ca0dc title) is WRONG outside ERR; the shipped feature requires
Oodle (runtime: GetModuleHandleW oo2core under Proton; offline: native liboo2corelinux64.so.9 or wine).
**PARSER CROSS-VALIDATED vs data/items_database.json (31089 baked entries, 504 maps):** my
parser's treasure count matches the DB's source=="treasure" slice — m10 DB treasure=107 ==
my with-part=107 (my 113 = 107 + 6 glow/no-part 0xFFFFFFFF events); overall mine 4075 ≈ DB
3986. The 7× apparent gap = OTHER sources the MSB Treasure event does NOT carry: enemy=25608
(ItemLotParam_enemy + enemy placement, separate RE), emevd=711+36 (scripted EMEVD grants),
None=748. **SCOPE LOCKED: MSB parser replaces the TREASURE slice of the bake (~4000), NOT
enemy-drops or EMEVD — those keep their baked/other source.** Confirms [[loot-identity-stable-err-additive]]
note that baked FUSES MSB-Events + EMEVD the runtime lacks.

**"NO POSITIONS IN VANILLA" RE'd + PROMPT WRITTEN (2026-06-24, docs/re/windows_no_position_lots_re_prompt.md,
commit 607cb65).** items_database.json has 748 entries with x=y=z=0/partName=""/source=None; all others
have a position (enemy pos from enemy MSB part c0000_xxxx). PROVEN these 748 lotIds are NOT in the MSB
bytes (m11 0/6, m20 0/66 found) → EMEVD-GRANTED, not MSB parts → that's why the MSB-part bake left them
position-less. 2 sub-classes: ~318 ERR-CUSTOM (map m60_44_60_00, lot 1044600000.., flag==lot,
Artifact-Piece/Dread-Essence = ERR-Reforged systems, locationless BY DESIGN) + ~430 scattered EMEVD
pickups (m11/m20/m12; armor sets share 1 ground-pickup flag; ~262 follow flag==lot+7000) where a
position MAY be recoverable. Windows-agent RE avenues (RPM/scan): EMEVD(mod/event/*.emevd.dcx, 517 files
KRAK)→entity/region→MSB POINT pos; runtime FieldIns/region/WorldChrMan; or existing WorldMapPointParam
row. EMEVD route needs Oodle (KRAK) — already proven callable.

**FULL LINUX VALIDATION A+B+C DONE for ERR (2026-06-24, the Windows agent built the in-DLL feature
`loot_from_disk_msb`; I reproduced on Linux).** Branch feat/msbe-disk-parser (linear: my 3 commits +
the agent's 12 on top, on origin). Part A offline: build.sh (DFLT) + native build_oodle.sh
(msbe_oodle_native.cpp, dlopen liboo2corelinux64.so.9 @ internals/launcher/, SysV ABI, NO Wine —
the agent's brief docs/linux_test_prompt.md referenced 2 build scripts that didn't exist + a
non-existent native harness; I wrote them). Part B: ALL 4 profile DLLs cross-build on Linux (cmake build-<p> clang-cl/xwin Release
-DGENERATED_SUBDIR=generated_<p> → ERR 5.16/erte 5.15/convergence 5.03/vanilla 4.89 MB PE32+). Non-ERR
needs the per-profile src/generated_<p>/ dirs (25 files each incl goblin_tile_tabs+goblin_major_regions,
git-ignored, Windows-pipeline-only) — user delivered them via mfg_generated_nonerr.zip 2026-06-24, no
stubs needed. ERTE+Convergence ALSO offline-parse-validated on Linux (maps at <downloads>/ERTE +
Convergence mod/{ERTE_mod,Convergence_mod}/map/MapStudio): 458/468 _00 maps exact-match Windows,
Convergence 122 treasure-KRAK maps prove native-Oodle-on-treasure-KRAK; vanilla offline blocked (Steam
BDT-packed, no loose maps). Part C non-ERR = needs per-profile ME3 launch setup (low value, loot path
profile-independent). AUTO-DETECT FIX shipped (commit 14897b5, resolve_map_dir walks up 5 levels from
DLL folder probing each ancestor + its mod/ subfolder + game-dir mod sibling) — VERIFIED in-game
2026-06-24: loot_msb_dir empty → DLL walks dll/offline→<root>→<root>/mod/map/MapStudio, logs "reading
MSBs from Z:\...\mod\map\MapStudio", identical 651/0/3235/21/3. No manual path needed anymore.

**PIVOT (2026-06-24): runtime auto-locate the mod data root, kill per-profile ME3 testing.** Instead of
launching each profile via ME3 (low value — Windows already ran all 4; the loot path is
profile-independent), find a STATIC MEMORY ANCHOR yielding the active mod's data root → map\MapStudio,
loader-agnostic (ME3/ModEngine2/UXM/vanilla), config-free. ME3 setup understood: me3 native runs on
Linux (`me3 launch -p <prof.me3> -g elden-ring --windows-binaries-dir bin/win64`), .me3 = TOML
[[natives]] (DLLs) + [[packages]] (mod folders), only err.me3/err_offline.me3 exist (non-ERR would need
new configs + separate savefile + dedicated DLL/INI — heavyweight, rejected). RE PROMPT WRITTEN
(docs/re/windows_regulation_modroot_anchor_re_prompt.md, commit 1614ab1): eldenring.exe exposes
CSRegulationManager + CSRegulationStep::STEP_WaitLoadLocalFile + Dantelion IO aliases
Core.IO.Alias.{regulation,mapstudio}/capture:/mapstudio → the virtual-FS alias/device resolver holds
the resolved mod root. Agent asked for anchor + offset chain (re_signatures.hpp AOB style), game-side
(loader-agnostic) preferred over reading ME3's own ME3_ATTACH_CONFIG/bridge structs. Fallback already
shipped = loot_msb_dir + ancestor-walk; possible CreateFileW/NtCreateFile/DLFileOperator hook if no
clean anchor. minhook infra present in DLL (overlay uses MH_CreateHook).
Part C Proton runtime: deployed DLL→dll/offline/, set [Goblin] loot_from_disk_msb=true +
loot_msb_dir=Z:\home\<user>\Games\ERRv2.2.9.6\mod (ER Proton prefix appid 1245620 maps z:->/).
[LOOTDISK]: 651 maps/0 KRAK skip/3235 replaced/21 disk-only/3 recover-later/Oodle available/825ms —
**EXACT match to Windows ERR run** → feature is platform-identical. ⚠️ AUTO-DETECT GAP: g_mod_folder =
the DLL's OWN folder (dll/offline, where the INI lives, set in dllmain) so resolve_map_dir() checks
dll/offline/map/MapStudio (absent) then eldenring.exe dir (BDT-packed, no loose) → MISSES the loose
mod/map/MapStudio; loot_msb_dir MUST be set explicitly on this ERR layout. Possible feature fix: also
probe ../../mod relative to the DLL. Status matrix = docs/loot_from_disk_test_status.md (ERR row ✅
A+B+C). Deploy is local to the game install (not committed); revert = loot_from_disk_msb=false or the
MapForGoblins.dll.bak-predisk-20260624 backup.
