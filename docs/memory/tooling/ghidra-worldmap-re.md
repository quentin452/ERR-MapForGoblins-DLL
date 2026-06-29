---
name: ghidra-worldmap-re
description: Ghidra setup + key world-map RE findings for eldenring.exe (live refresh + map cursor)
metadata: 
  node_type: memory
  type: reference
---

**Ghidra** 12.1.2 at `D:\ghidra`. Analyzed `eldenring.exe` project persisted at `<ghidra_project>\ER`
(~1.4 GB, the 2 h auto-analysis is saved) — **re-run scripts in minutes** without re-analysing:
```
GHIDRA_HEADLESS_MAXMEM=16G analyzeHeadless.bat <ghidra_project> ER -process eldenring.exe ^
   -noanalysis -postScript <name>.java -scriptPath <ghidra_scripts>
```
Scripts must be **Java GhidraScripts** — Ghidra 12 dropped Jython (Python needs PyGhidra). Recon
scripts live in `<ghidra_scripts>` (`find_worldmap.java`, `re_v2..re_v8.java`). Invoke via the
PowerShell tool (not Bash `cmd /c`). Imagebase `0x140000000`; analyse the real `.text` at
`0x140001000`, NOT the VMProtect `.text` at `0x144c0e000` (its "failed to create function" noise is
expected). RTTI mangled names (`.?AV…@CS@@`) are referenced by 32-bit RVAs, so resolve vtables by
scanning for the TypeDescriptor RVA → COL → vtable (see `re_v4.java`'s `rttiWalk`).

**Findings** (full doc: `docs/world_map_re_findings_windows.md`). For the section-toggle live-refresh
(`feat/section-toggle`) + clustering (`feat/clustering`):
- `CSWorldMapPointMan` FD4Singleton instance slot = **`0x143d6e9d8`** (AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 75 05 E8`).
- **Live-refresh candidate = `FUN_140a832a0`** (RVA `0xa832a0`): iterates the manager point list,
  calls `_DiscoverMapPoint` (`FUN_140a84080`) + per-point ops → the refresh/re-eval loop.
- `CSWorldMapPointIns` vtable `0x142b487a8`; `_Verify{Enable,Disable}EventFlag` = `0xd58640`/`0xd58550`;
  per-row predicate `0xd58470`.
- **Map cursor:** `WorldMapCursorControl` is **embedded in the world-map menu at `menu+0x2DB0`**;
  position floats at `+0xFC / +0x104 / +0x10C` (clamped to bounds rect `[menu+0x2DB0+0xF0]+0x340..0x34C`
  = marker space). vtable `0x142b29a90`, Update method `FUN_1409bd4b0`, menu setup `FUN_1409be5e0`.
- **Still needs the running game** (<user> does runtime — see [[workflow-preferences]]): confirm
  `FUN_140a832a0` re-renders after the `areaNo`-99 flip (else fallback = programmatic reopen via
  `FUN_1409be5e0`/CSMenuMan `+0xCD==7`); grab the menu instance pointer + confirm cursor X-vs-Z.

**Cursor-focus follow-up (Target B, scripts `re_v9..v12.java`; full doc `docs/re_findings_questbrowser_cursor.md`):**
cursor class RTTI-confirmed `CS::WorldMapCursorControl` (vtable `0x142b29a90`, 3 vmethods: vt2=`FUN_1409bd4b0`
tick). Coords `+0xFC/+0x104/+0x10C` are **writable** (ctor `FUN_1409bc5b0` + tick write them). View pan =
`FUN_1409bdc50` (edge-scroll); snap-to-target lerp = `FUN_1409bc8c0` (snap-state fields +0x118/+0x120/+0x134/+0x130).
World-map **singleton ptr @ `0x143d5dea8`** with a `[+0x883]` map-open-guard candidate (NOT confirmed ==
CSWorldMapMenu, NOT shown to own the cursor — chain root still open). Live-verify recipe + tools (probe
`debug_worldmap_probe`→`logs/MapForGoblins_wmprobe.log`, then Cheat Engine write +0xFC) in
`<windows_downloads>\MapForGoblins_verify_cursor_recipe.md`.

**Target A — player pos (static, app 2.6.2.0; scripts `re_v13..v18.java`; doc `docs/re_findings_playerpos.md`):**
old CT `LocalPlayerOffset 0x10EF8` is **DEAD** (drifted; 1 occurrence in the binary). WorldChrMan static =
RVA **`0x3d65f88`** (AOB `48 8B FA 0F 11 41 70 48 8B 05` `{{0xA,0xE}}`, confirmed). The engine builds the
**player position in MARKER space** (= WorldMapPointParam.posX/posZ, no chunk→world bridge needed): new player
field `[WCM+0x1e508]`; `FUN_1406d3a20` (0x6d3a20) writes it to **manager+0x70(X)/+0x78(Z)**; manager =
map-point/discovery singleton updated by `FUN_1406d31f0`(0x6d31f0)←`FUN_140623410`, candidates
`0x3d692f8/0x3d69380/0x3d69ba8`. Live TODO: pin the manager + confirm cross-block continuity (recipe
`<windows_downloads>\MapForGoblins_verify_playerpos_recipe.md`).

**Player-pos — STATIC CLOSED (CE manager pinned, scripts `re_v19`/`re_v20`, 2026-06-18):** live CE pinned candidate
**`eldenring.exe+0x3d69ba8`** (P=`0x1BAA164E680`, `P+0x70`/`P+0x78` floats = −2.77/−24.08). Decompiled the FULL
builder→reader→projector path: **it is a pure Vec copy, ZERO arithmetic** — projector `FUN_14045e390(src,out)` just
copies `src+0x70/+0x78`; reader `FUN_1403c6ff0` src = `[[[ [player+0x58]+0x10]+0x190]+0x68]`; builder `FUN_1406d3a20`
writes manager+0x70/+0x78 from it. **No `256`, no `1/256`, no floor, no gridX/Z field anywhere** (only timer floats +
discovery-count ints). So manager+0x70/+0x78 = **raw block-local physics Vec**, NOT marker space — the old "already
in marker space, no bridge" claim was WRONG. ⚠️ Correction to earlier note: marker `posX/posZ` are **signed &
unnormalized** (CSV range ≈ −880..+1620, NOT ±500), so there is **no scale factor** — CELLSIZE is just **256**.
The full marker transform already lives in the repo: `tools/extract_markers.py` (grace-verified): `worldX=mapX+7042`,
`worldZ=-mapZ+16511`, `gridX=worldX//256`, `posX=worldX-gridX·256` → marker world = `gridXNo·256+posX`.
**FORK RESOLVED — block-local confirmed (live, 2026-06-18):** walked watching pinned `P+0x70/+0x78`. Axes: `+0x70`=X
(E+), `+0x78`=Z (N+). **Sawtooth amplitude ±32 (NOT ±128)**, resets every chunk boundary (N: 0→32→0→32; S:
0→−32→0→−32). **Directional hysteresis** (go N `…32→0`, reverse → `0→−32` not `0→32`) = the field is player pos LOCAL
to the **current parent world-block**, streaming **re-parents** at boundaries (sign flips with entry edge) → local is
NOT a pure function of position. `re_v21`: NO scale constant anywhere (the ±32 is the physics block's own half-size
~64u; local↔posX likely 1:1 in units); alt/fallback branches are just more `+0x70/+0x78` getters (no cleaner absolute
field); block system = `CSWorldBlockGeomUpdaterMT::AddBlock` in `FUN_1406d2d80` (RVA 0x6d2d80). **Final bridge plan
(empirical):** (1) CE pin the int that does **+1 per boundary** = player tile/block idx (X & Z) — "increased value by
1" scan; (2) stand on ONE known grace, read player local + idx, compare to that grace's CSV `posX/posZ` + `gridXNo/
gridZNo` → gives BOTH unit-scale and idx↔gridXNo relation (idx the 256-tile, or 64u sub-block ⟹ //4) in one shot; (3)
`markerX = gridXNo·256 + posX`, clustering proximity direct (no global continuity needed). Full writeup:
`docs/re_findings_playerpos.md` (LIVE + v21 + Final bridge plan sections).
**Live gridZ hunt = painful/not landed (2026-06-19):** Exact-Value narrowing works in principle (gridZNo IS a plain
4-byte int; pinned once @ dynamic addr then lost on CE restart) but needs a BIG overworld-North jump (gridZ ticks per
256 only; First Step↔Murkwater Catacombs = same latitude + catacomb freezes overworld gridZ → no change). **SHORTCUT
LEAD to try first:** a teleport/warp cheat-table reads/writes the player pos — if those warp coords are GLOBAL world
(thousands, continuous) they ARE Target A (skip the hunt). Test: read CT coord at The First Step (tile 42,36) → expect
~X≈10740/Z≈9263 if world-space; if still ±32+MapId, the CT gives us the block-origin table. Capture the CT pointer
path. **Bonus live finding:** "Show All Grace" rebuilds the OPEN map in real time → on-demand icon-rebuild path for the
section-toggle live-refresh thread (drive `FUN_140a832a0`/`_DiscoverMapPoint`); logged in `docs/world_map_live_refresh_re.md`.
**SOLVED (2026-06-19, re_v21–v30, full writeup in `docs/re_findings_playerpos.md`):** ER stores NO global float — pos =
block-local + block identity. **MapId 4-byte layout:** byte[3]=area, byte[2]=gridX, byte[1]=gridZ, byte[0]=lod (First
Step=`0x3C2A2400`). **Block→world bridge** `FUN_1408775e0`: world = local + node[+0x24/+0x28/+0x2c] (block origin), MapId
@node+0x1C, parent @node+0x20. **Player→map-UI transform** `FUN_140876140`: `world = gridXZ·256 + local`, CELLSIZE=256.0
(DAT_1429ce8b4) confirmed, Z-flip 0x80000000 (DAT_14329f470). **Player local CONFIRMED stable:** `[eldenring.exe+0x3d69ba8]
+0x70`=X, `+0x74`=Z, `+0x78`=height (NOT +0x78=Z as old note said). WorldChrMan=`eldenring.exe+0x3d65f88`.
**Player MapId pointer CONFIRMED (not fully solved):** `[[eldenring.exe+0x3d691d8]+0x2c]` (4B) = player IMMEDIATE-block MapId.
Live: First Step `0x3C2A2400`=(42,36)✓ exact, Castle Morne `0x3C2B1F00`=(43,31)✓ exact, **Academy Gate `0x3C232E00`=(35,46)
but icon tile=(37,44) → off (+2,−2).** So immediate-block grid ≠ icon/display grid in NESTED regions (Liurnia). Game transform
`FUN_140876140` re-parents up the block tree (`FUN_1408775e0`, accumulate `node+0x24/+0x2c` origins, parent @node+0x20) → gridX/Z
from TOP-LEVEL block; Limgrave=identity (matched), Liurnia nested. **To finish:** replicate re-parent over block tree at `mgr=
[eldenring.exe+0x3d69ba8]`, OR derive per-region offset table, OR pin the map-open player map-UI cache (the `+0x3d6b7b0` chain's
+0xAC/0xB0 were map constants, not it). local=`[+0x3d69ba8]+0x70`(X)/`+0x74`(Z)=block-local. Full detail in `docs/re_findings_playerpos.md`.
**Tooling:** Ghidra headless works — `D:\ghidra\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat "<ghidra_project>" ER -process
eldenring.exe -noanalysis -scriptPath <ghidra_scripts> -postScript re_vNN.java` (~2min/run); scripts re_v21..v30 in <ghidra_scripts>.
**Dead end checked:** `<windows_downloads>\ergg…\ergg.dll` ("Gold Gaoler Overlay", proprietary, all-rights-reserved) =
player-list + block-player ONLY; its "distance" strings are zlib internals; **no player-pos/distance formula**
(only confirms it resolves WorldChrMan by AOB on 2.6.2.0). Ghidra import at `D:\ghidra_proj_ergg`; RE scripts
`re_v9..v18` + `find_ergg.java` live in `<ghidra_scripts>`.

**"Show All Graces" AOB = DEAD END for icon refresh (2026-06-19, re_v31..v37, doc `docs/windows_re_live_refresh_grace_lead.md`):**
The CT `NewMenuSystemWarp2` AOB (`0F B6 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 8B E0`) resolves to:
site VA `0x140889111`; toggle byte **`0x143d6cfc0`** (RVA 0x3d6cfc0); E8→**`FUN_1408882d0`** (0x8882d0);
containing **`FUN_1408890b0`** (0x8890b0, caller FUN_140887870). It is the **map-FRAGMENT/overlay reveal**
path (`WorldMapPieceParam` + `CS::WorldMapDialogBase::_IsChangeableOverlayLayer`/`GetEnableMapNoMask`),
NOT points. Byte≠0 ⇒ FUN_1408882d0 returns 0xffffffff (all pieces). Bridge check: ZERO refs from any of
the 7 byte-consumers to the point subsystem (0xa832a0/0xa84080/0x143d6e9d8). So flipping it reveals map
TERRAIN live, does nothing for our `WorldMapPointParam` icons. "Show All Graces" = misnomer.
**Real point pipeline (separate):** per-frame `FUN_140623410` (0x623410, FD4Time delta) → **`FUN_140a832a0`**
(0xa832a0) = per-frame RECONCILE of already-built `CSWorldMapPointIns` RB-tree ([singleton+8], color @+0x19),
reads player pos [DAT_143d65f88+0x1e508], calls inst vmethods +0x18/+0x28(→+0x90), _DiscoverMapPoint 0xa84080,
add 0xa84210, remove 0xa850c0. It does NOT rebuild from params. **Build-from-params** = constructor
`FUN_140a811e0` (0xa811e0) new-ed at site **`0xa82d09`** (in an UN-analyzed gap between FUN_140a82a80 and
FUN_140a82eb0 — Ghidra never made it a fn); = analog of piece path's FUN_1408890b0. **Pin its entry at RUNTIME**
(bp the constructor 0xa811e0, read caller). Next runtime steps: (1) re-drive that build loop after areaNo flip
while map open; (2) test if areaNo→99 hide already works live via FUN_140a832a0 predicate; (3) fallback = prog reopen.

**LIVE-REFRESH SOLVED STATICALLY + SHIPPED (2026-06-19, re_v38–v44, branch `feat/live-refresh-world-map-icons`,
full writeup `docs/windows_re_live_refresh_capture.md` RESULTS):** The build fn is **`FUN_140a82a80` (0xa82a80)** —
Ghidra had truncated it to 160B because allocator `FUN_141eb9ed0` was mis-flagged noreturn (clear it →
`f.setNoReturn(false)` to recover the full body). Sig `void __fastcall(PointMan* this/rcx, ctx* rdx)`; ctx =
transient per-frame `{+0x34 int pageFilter, +0x48 ParamRepo*}` (so NOT fabricatable). Built-icon container =
**`std::map<int,CSWorldMapPointIns*>` @ `PointMan+0x398`** (node key@+0x20, value@+0x28, color@+0x19); 2nd tree
@`+0x8` = discovery/reentry set (only set the per-frame reconcile FUN_140a832a0 add/removes). **The +0x398
reconcile only UPDATES icons (`thunk_FUN_1457cd3df`), never re-runs the show-predicate (vtable+0x8) → live areaNo
edit invisible, AND no per-frame instance field to flip (4b impossible).** Instance: 0x110B, vtable 0x142b487a8,
param row @`inst+0x80`, sub-obj @`+0x18`, vt+0x8=show-predicate FUN_140a81450, vt+0x18=area getter FUN_140a81440,
vt+0x28=FUN_140a811d0. PointMan instance static = `0x143d6e9b0` (build driver `FUN_14063d400` calls
`FUN_140a82a80(*0x143d6e9b0, *(arg+8))` every world-step). Reopen = WorldMapDialog ctor `FUN_1409cef10`→setup
`FUN_1409be5e0` pushed by factory @0x9cfa05 (no clean reopen() call). **Shipped: hook-capture-replay** — hook
FUN_140a82a80 (entry AOB unique `40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF FF FF
4C 8B F9 8B 42 34`), detour replays original with engine's own captured (this,ctx) on request, gated by config
`live_refresh_world_map` (default OFF). **RUNTIME-UNVERIFIED** (no MSVC on this box — <user> builds/tests): key
question = does FUN_140a82a80 run while the map is OPEN (if the world-step driver pauses under a fullscreen menu,
replay won't fire until close → degrades to today's reopen behavior, no regression). Scripts re_v38..v44 in
<ghidra_scripts>.

**WORLD→SCREEN PROJECTION RE'd statically (2026-06-19, re_v45–v50, full doc `docs/world_map_projection_re_findings.md`):**
The map is rendered by **Scaleform (GFx/Flash)** — there is **NO flat C++ view matrix/center/zoom** for the final
marker→pixel step (that's why the flat float-dump never found it). Forward = `Scaleform::Render::Matrix2x4<float>`
(twips, ×20 px) inside the movie (`FUN_140d82070`/`FUN_140d7ff40`; matrix getter `FUN_14117d180` 0x117d180).
**BUT the live pan/zoom ARE plain floats**, just behind the `cursor+0xF0` pointer (a `CS::WorldMapArea`, ctor
`FUN_1409cb9c0` 0x9cb9c0): **`+0x378` = pan (vec2 marker space), `+0x380` = zoom (f32).** Transform =
`view_local=(marker+pan)/zoom` (`FUN_1409cd0a0` 0x9cd0a0) then GFx → **virtual 1920×1080 canvas**
(`[eldenring.exe+0x47ef360]+0x128` → +0x110/114 origin, +0x118/11c W/H; hard-coded 1920/1080 in pan fn
`FUN_1409bdc50` + pan-bounds `FUN_1409ce190`) → backbuffer (×realW/1920, realH/1080). `WorldMapArea+0x350..0x35c`
= STATIC full-map rect `[0,0,10496,10496]` (NOT viewport — don't use for zoom); `+0x340..0x34c` = snap rect;
`+0x6e`=areaNo. Scaleform fit math = `FUN_140d84990` (0xd84990), wrapper `FUN_140d82770`. **Current page:**
`menu+0x151` (page id via `FUN_1409c4900`), `DAT_143d6cfc3` (u8, underground/sublayer flag), per-page transform.
**RECOMMENDED impl = analytic affine** `canvasX=(markerX+panX)/zoom · realW/1920 (+ calibrated bias)`, reads only;
fallback = call engine projection on the render thread. **SHIPPED instrument:** extended `debug_worldmap_probe`
(`goblin_worldmap_probe.cpp`) to follow +0xF0 and log pan/zoom + canvas on cursor-move — **make-or-break runtime test
(<user>):** open map → PAN should sweep +0x378, ZOOM should change +0x380, +0x350 rect stays fixed; then a known
grace's `world_to_screen` should land on its icon through pan+zoom. Confirm +0x378 x/z order live.

**MARKER row → RENDER space RE'd (2026-06-20, re_v51–v55, doc `docs/marker_to_mapspace_re_findings.md`):**
The (areaNo,gridX,gridZ,posX,posZ) → render-marker-space (cursor space, WorldMapArea fullRect [0,0,10496,10496])
transform is a **per-PAGE affine**: `render = (gridXZ·256 + pos − origin[dstPage]) · 0.5`. SCALE=0.5 structurally
confirmed (10496 = 82 tiles × 128 render-units; world cell 256 → render 128). **origin is per dst page** (60/61/12/
40-43 each distinct): measured origin₆₁ₓ≈300 (Dragonbarrow area21→**dst 61**, grid48 world12338→render6018.75),
origin₆₀ₓ≈5632 (Academy Gate area16→dst60, grid36 world9218→render1826) — so NO single global transform.
Legacy dungeons project to dst page via **WorldMapLegacyConvParam as a base-point TRANSLATION** (dstWorld =
dstBaseWorld + (srcWorld − srcBaseWorld)), NOT dst-substitution — the mod's current `project_dungeon_row_to_overworld`
substitutes dst_gx·256+dst_pos & keys only on (src_area,src_gx), dropping the offset+srcGridZ = the area-16 "wrong
region" bug. **origin constants NOT liftable statically** (placement is Scaleform `thunk_FUN_1457cd3df` → VMP region;
CSWorldMapPointIns has 7 vmethods, vt[4] FUN_140a81140 = MapId getter not coord; build/reconcile carry no coord math)
→ deliver origin[page] as a runtime-calibrated table (1-2 graces/page; overlay calib UI already does per-region tuning).
Possible per-page Z-flip (player→map-UI FUN_140876140 has 0x80000000 flip). Then screen = (render−viewCentre)·zoom+canvas/2
(SOLVED, projection doc). User has overlayMarkersProto + get_live_view prototype already projecting this empirically.
**UPDATE (2026-06-20): by-hand calib FAILED — constellation comes out ROTATED → transform is full affine `render=M·world+T`,
M=scale·ROTATION (not diagonal). Single anchor (Dragonbarrow p61) fits a 90° AXIS-SWAP @0.5: renderX≈0.5·worldZ+e,
renderZ≈0.5·worldX+f, with tiny per-page T (Tx61≈26,Tz61≈18). M shared, T per page. Can't solve M from 2 anchors on 2
diff pages → need ≥3 same-page graces. Delivered `tools/MapForGoblins_mapspace.CT` (CE table: auto-resolves live cursor by
vtable-scan → render +0x104/+0x108, WorldMapArea pan/zoom/fullRect, page menu+0x151; NUMPAD0 snapshot logger →
MapForGoblins_mapspace_ct.log) + `docs/marker_mapspace_CT_recipe.md` (hover-known-graces → lstsq solve M+T, ready area-60/61
anchors, axis-swap candidate). Placement hook = thunk_FUN_1457cd3df (VMP) optional.**
**Placement-hook site PINNED (2026-06-20, re_v56, doc `docs/marker_affine_hook_re_findings.md`):** to capture clean
`world_in` for the M·world+T fit, hook the `CALL thunk_FUN_1457cd3df` in reconcile FUN_140a832a0 @ **RVA 0xa839a6**
(AOB `66 48 0F 7E C9 48 85 C9 74 08 49 8B D5 E8`, call at found+13). **RCX = CSWorldMapPointIns* (point), RDX = ctx**;
param row @ **point+0x80** → world=(gridX·256+posX, gridZ·256+posZ). **render_out is NOT on `point`** (ctor's +0xa0/+0xe0
are a COLOR/UV LUT via FUN_1401899c0, not coords) — it's on the icon's Scaleform Matrix2x4 written by the VMP thunk
(entry 0xa805e0). Get render_out via (1) in-thunk matrix-write bp, OR (2) cursor snap cross-check (cursor +0x104/+0x108
IS exact when snapped — the snap target = icon render pos, so the hover recipe isn't actually noisy if you let it snap).
Overlay already has g_aff/solve_affine wired.

**★ MARKER→MAPSPACE FORMULA SOLVED — ONE conversion, exact game constants (2026-06-20, doc
`docs/marker_to_mapspace_re_findings.md` CORRECTED section).** The per-dst-page-origin theory was
WRONG (noisy fit). Transform = **`FUN_140876140`** (RVA 0x876140), **diagonal + Z-flip, NO swap/rotation**:
`mapX = (worldX−originX)·scale + biasX ; mapZ = −(worldZ−originZ)·scale + biasZ` (worldX=gridX·256+posX,
256=DAT_1429ce8b4, Zflip=DAT_14329f470=0x80000000). Converter element fields: area+0xB, gridXbase+0xA,
gridZbase+0x9, originXf+0xC, originZf+0x14, biasX+0x18, biasZ+0x1C, scale+0x20, blocknode+0x28.
Converters live INLINE in **CS::WorldMapViewModel** (vtable RVA 0x2ad82e0): array @ VM+0xF8 stride 0x30,
count @ VM+0x280. **Live dump (tools/cheat_engine/MapForGoblins_converter_dump.CT):** area 60 AND 61 =
IDENTICAL (scale 1.0, origin 7168/16384, bias 128/128) → **mapX=worldX−7040, mapZ=−worldZ+16512** =
ONE overworld conversion (matches repo extract_markers.py 7042/16511). **Underground SHARES it** (live:
Siofra/Ainsel on screen, converters still area 60/61, no area-12 loaded) → one formula for OW+UG. scale=1.0
= world→mapspace stage; mapspace→screen (×0.5/pan/zoom) is the separate WorldMapArea step. **DLC (page 10,
area 40-43) still to dump.** find_formula.java + converter-dump CT are the RE tools.

**CURSOR O(1) CHAIN — static side closed, runtime field remains (2026-06-20, scripts `find_chain.java`..`find_chain4.java`,
out_chain*.txt; brief `docs/windows_worldmap_cursor_o1_re_prompt.md`):** Resolving the brief from commit c15bcc0.
- **cursor vtable RVA `0x2b29a90` STILL VALID on 2.6.2.0** — Ghidra labels it `CS::WorldMapCursorControl::vftable`;
  RTTI `.?AVWorldMapCursorControl@CS@@` @ `0x143cddb30`. No drift.
- **cursor = dialog + 0x2DB0 — TRIPLE-confirmed**: base ctor **`FUN_1409c1080`** calls cursor ctor as
  `FUN_1409bc6a0(param_1 + 0x5b6)` and param_1 is `undefined8*` ⟹ `0x5b6·8 = 0x2DB0`. Cursor ctors = `FUN_1409bc6a0`
  (the +0x2DB0 one) + `FUN_1409bc5b0`.
- **The "world-map menu" = `CS::WorldMapDialog`** (derived, ctor **`FUN_1409cf8f0`** sets `WorldMapDialog::vftable`,
  sizeof `0x3ed0`) over base **`CS::WorldMapDialogBase`** (`FUN_1409c1080`). 2nd variant ctor `FUN_1409c1c10` (sizeof `0x3ec8`).
- **CSMenuMan static slot = `0x143d6b7b0`** (RVA 0x3d6b7b0; AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` disp@3 len7,
  SINGLE unambiguous match @ `0x140758056`). NB: this is the same `+0x3d6b7b0` chain noted dead for player-pos above.
- **CSMenuMan→dialog offset is NOT statically resolvable** = genuine runtime field: CSMenuMan has **471 reader funcs**
  (hot global), the WorldMapDialog vtable is an unnamed `DAT_` (only 4 debug-string users, none are CSMenuMan readers),
  and the concrete dialog ctors have **no static callers** (created via factory/vtable table). Reader-∩-dialog-user = ∅.
- **PRAGMATIC FIX for the brief's real goal (kill the multi-GB scan):** replace whole-RW scan with a **bounded walk of
  CSMenuMan's struct** — read `mm = *(0x143d6b7b0)`, scan its first ~few-KB qwords for a ptr `p` with
  `*(p+0x2DB0) == base+0x2b29a90`; that's O(KB)/frame not O(GB) ⟹ instant + lag-free + patch-stable. Log the winning
  offset at runtime → becomes the hardcoded O(1) deref for that patch. **`CSMenuMan+0xCD==7` gate still needs runtime
  reconfirm on 2.6.2.0** (not checkable statically). See [[mapforgoblins-pipeline-setup]], [[workflow-preferences]].

**OPEN-MAP REGION field — static side (2026-06-20, scripts `find_region.java`/`find_region2.java`,
out_region*.txt; brief+RESULTS `docs/windows_open_map_region_re_prompt.md`):** For overlay region gating
(draw only the viewed map's graces). Cursor O(1) walk SHIPPED by <user> (`f0512ef` 2-level CSMenuMan walk,
`1fc4b01` removed the scan) — `dialog = cursor−0x2DB0`, `view = *(cursor+0xF0)`.
- **Authoritative open-page field = `dialog + 0xA88` (int32)** = the `[0x151]` menu page id (undefined8*
  units ⟹ byte 0xA88). Written by setup `FUN_1409be5e0`(0x9be5e0) L259 from the open param + on page-change
  via `FUN_1409c8120(dialog,page)`; decoded by page→mapno getter **`FUN_1409c4900`(0x9c4900)** (returns page
  as-is, page==1→_DefaultMapNo, neg→0). View-driven (menu-selected page, not player area). **Try this FIRST.**
- **Secondary: `view+0x370`/`+0x374`** = WorldMapArea ctor (`FUN_1409cb9c0` 0x9cb9c0) params 5/6
  (`+0x6e`*8=0x370). delta-scan `0↔10`/`1↔11`. One area per dialog @ dialog+0x4fb; ctor-arg constants
  decompiler-truncated ⟹ NOT statically liftable.
- **Dead ends reconfirmed:** `FUN_140d58470` (brief's "show predicate") = grace event-flag check
  (_Verify{En,Dis}ableEventFlag 0xd58640/0xd58550), NOT region gate. `FUN_1409c6f70` = overlay-layer
  changeability (_IsChangeableOverlayLayer, reads overlay toggle `DAT_143d6cfc0`), NOT region. Sublayer byte
  `DAT_143d6cfc3` already live-dead (always 0).
- **Value→region = RUNTIME** (<user>'s flow): read `dialog+0xA88`, `view+0x370`, `dialog+0x98` on the 4 maps
  (OW/UG/DLC-OW/DLC-UG), pick the one with 4 distinct values (or a (page,sublayer) combo — UG may need it).

**OPEN-MAP REGION — page fields FAILED live, pivot to overlay-LAYER (2026-06-20, scripts
`find_cursor_area.java`..`find_cursor_area5.java`, out_cursor_area*.txt):** Live test of the region CT
(`tools/cheat_engine/MapForGoblins_region.CT`): **dialog+0xA88 page UNUSABLE** — Overworld==Underground
(same value), and the dialog/view fields get POLLUTED when another dialog opens on top (transient dialog,
not stable). Only DLC differed, but fragile. So page-id is NOT the region key.
- **Cursor has NO stored tile/areaNo** (user hypothesis answered NO): cursor vtable = 3 methods only
  (dtor `FUN_1407342b0`, vt1 `FUN_1409bc730`, tick `FUN_1409bd4b0`); tick does pan/reticle-clamp only.
  Cursor stores render coords (+0xFC/+0x104), area is resolved ON DEMAND, never cached.
- **World-map singleton `DAT_143d5dea8`** (cursor tick reads +0x883 = map-open guard): state cluster
  +0x880..+0x888 = scroll-pos(+0x880/+0x884, scaled by DAT_14329e63c) + flags(+0x881/+0x882/+0x883/+0x888).
  NOT region. (serializer FUN_14095b990).
- **True region key = the MapId AREA byte** (packed MapId byte[3]: 60/61=OW, 12=UG, 40-43=DLC). Resolver
  subsystem (RTTI-walked): WorldMapLegacyConverter vtable `0x142ad5998`, WorldMapTileBackReader `0x142ad7b10`,
  WorldMapTileRes `0x142ad7be8`, WorldMapPlaceNameParam `0x142ad61d0`; forward transform **`FUN_140876140`**
  (world=gridXZ·256+local) gates on area byte (`MapId>>0x18 == converter+0xB`). The INVERSE (cursor render
  pos→MapId/area) is NOT a tidy single fn (forward is per-area-gated). To capture hovered areaNo at runtime,
  bp `FUN_140876140` / the place-name update and read the MapId arg.
- **NEW LEAD for OW-vs-UG (untested): `DAT_143d6cfc0` (RVA 0x3d6cfc0, u8) = overlay/map-VARIATION LAYER**
  (the `_IsChangeableOverlayLayer`/`_GetMapVariation` byte, read by FUN_1409c6f70/FUN_1408882d0) — DISTINCT
  from the dead sublayer byte `DAT_143d6cfc3`. In ERR underground is an overlay LAYER on the same canvas
  (why OW==UG page). **Full region key likely = (page=dialog+0xA88, layer=DAT_143d6cfc0):** page splits
  base/DLC, layer splits surface/underground. Added to the region CT (entry 8) — <user> to live-test.
- **AGREED FALLBACK if the layer byte fails (<user>'s idea, 2026-06-20): RE the MAP-ASSET LOAD path.**
  Game loads tiles per-region → find the active `WorldMapTileRes` (vtable `0x142ad7be8`, carries the loaded
  mapId → area byte) or current `WorldMapTileBackReader` (vtable `0x142ad7b10`, reads tile/mapId at a pos).
  Find the loader/slot holding the ACTIVE tile-res (singleton or menu/dialog field), read its mapId =
  ground-truth region, independent of the leaky dialog fields. Converter = `WorldMapLegacyConverter` vtable
  `0x142ad5998`. Start there next session if the (page,layer) key doesn't hold.
- **LAYER byte `DAT_143d6cfc0` also DEAD (always 0, live 2026-06-20).** Map-asset path RE'd (find_tile_asset1..4):
  owner = **`WorldMapMan`/`WorldMapManImp`** (RTTI `.?AVWorldMapManImp@CS@@`, vtable `0x142a8f918` [thin, dtor
  only], ctor `FUN_1407309f0`, ~105KB struct, arrays stride 0x220/0x28/0x40) = a per-POSITION tile STREAMER
  (`WorldMapMan_Update`/`WorldMapMan_BackreadRequestUpdate`) — NO single current-region int. `WorldMapTiledLayer`
  vtable `0x142b2caf0` (dtor `FUN_1409cc250`, in the dialog cluster). ManImp ctor has no static callers (FD4Singleton).
- **VERDICT: ER stores NO single "open-region" field** — unified streaming canvas, region resolved per-position
  on demand. 9 candidates dead/leaky (page 0xA88, view 0x370/0x374, dialog 0x98/0x14, cfc0/cfc3, DAT_143d5dea8
  cluster, WorldMapMan). DLC IS separable via page; only OW-vs-UG lacks a stored bit. **Definitive path =
  RUNTIME hook on the TILE BACK-READER** (`WorldMapTileBackReader` vtable `0x142ad7b10`, driver
  `WorldMapMan_BackreadRequestUpdate`): resolves cursor/view pos → tile → MapId; read area byte (>>0x18) =
  60/61 OW / 12 UG / 40-43 DLC. NOTE `FUN_140876140` is PLAYER→UI (player-driven) — NOT the open map; need the
  INVERSE (cursor→MapId) = the back-reader. Back-reader READ method (sig pos->mapId) still needs pinning (1 pass).
- **HOOK ATTEMPT FAILED — no clean hookable resolver (find_backread/find_placename, 2026-06-20):** back-reader
  is a thin `DLFixedVector<tile>` (ctor FUN_140882500, vtable dtor-only) — read not in call graph. `FUN_140876140`'s
  8 callers (FUN_140888d50/886c40/8877d0/8855b0/887150/886b10/8876c0/887870) all pass the converter @+0xf8 to
  PLACE elements (markers/player), none resolve the cursor. Place-name fns = MsgTagManImp ctor (FUN_140d11440) +
  generic msg lookup (FUN_140d10b60) — not area resolvers. **12 passes total: ER has NO clean static region field
  AND no tidy cursor->MapId hook.** PIVOT to EMPIRICAL CE hunt: added `mfg_areahunt()` (NUMPAD 2) to the region CT
  — scans cursor[0..0x400]/view[0..0x400]/dialog[0..0x1400] for packed-MapId int32 (top byte=area) + lone area
  bytes (60/61/12/40-43), logs to MapForGoblins_areahunt.log. Run on OW then UG, diff → the offset whose area
  flips 60/61->12 is the field (if one exists). DLC already gated via page. If areahunt finds nothing stable,
  region gating ceiling = base-vs-DLC (page), OW/UG shown together.
- **SOLVED via <user>'s clue (the 3 map-switch buttons → `WorldMapSwitchDialog`, 2026-06-20, scripts
  find_mapstrings/find_switchdialog/find_variation/find_cfc3write):** The map offers 3 toggle buttons (Lands
  Between / Realm of Shadows / Surface↔Underground) = **`CS::WorldMapSwitchDialog`** (RTTI `.?AVWorldMapSwitchDialog@CS@@`
  @ `0x143ce27f0`, vtable `0x142b30d80`, ctor `FUN_1409deb60`, struct 0xa88, selection @+0x14b). **OW/UG KEY =
  `DAT_143d6cfc3`** (RVA 0x3d6cfc3, u8): 0=surface, !=0=underground. **WRITTEN at map-build by `FUN_1408855b0`**
  (`DAT_143d6cfc3 = thunk_FUN_144dc036a()` + sibling `DAT_143d6cfc2 = thunk_FUN_1459a846c()`), and the **WorldMapArea
  projection BRANCHES on it**: `FUN_1409ce190`(pan-bounds)/`FUN_1409cd790`/`FUN_1409ce4b0` do `cfc3==0 ? surface rect
  view+0x360/0x370 : underground rect view+0x350/0x358`. So cfc3 IS the surface/underground discriminator — earlier
  "cfc3 dead, always 0" was a BAD TEST (never actually entered underground; it's set when the map BUILDS in UG mode
  via the 'Afficher souterrains' button, not by panning). **`DAT_143d6cfc0` CONFIRMED DEAD** (read-only, zero writers).
  **FULL REGION KEY = `(page=dialog+0xA88 [base vs DLC], cfc3=DAT_143d6cfc3 [surface vs underground])`** → all 4 maps.
  CT updated (entry 7 cfc3 relabelled as the key, entry 9 cfc2). Re-test: click 'Afficher souterrains', watch 0x3d6cfc3 flip.
- **cfc3 THEORY FAILED EMPIRICALLY (live 2026-06-20, 4-map snapshots).** OW and UG are BYTE-FOR-BYTE IDENTICAL in
  EVERY probed field: page[A88]=0, view370=0, view374=1, d98=1, d14=0, **cfc3=0**, cfc2=0, cfc0=0 — cfc3 does NOT
  flip for ERR underground (the FUN_1408855b0/thunk_FUN_144dc036a path isn't taken; the projection branch exists in
  code but the flag stays 0). d5c is noise (0 vs 442 across two OW reads). **DLC cleanly separable: page[A88]=10,
  view370=10, view374=11 (base=0/0/1).** **VERDICT: ERR has NO world-map UI-state field separating Overworld from
  Underground** — opening UG changes only the RENDERED tiles (and likely WHERE on the 10496 canvas the view sits),
  not any dialog/view/global byte. base↔DLC SOLVED (page/view370 = 0 vs 10); OW↔UG only-remaining-signal = the
  cursor PAN(+0x378)/RENDER(+0x104) position (if UG is a different canvas region). CT snapshot extended to log
  pan/zoom/render for an OW-vs-UG canvas-region test. If those match too → OW/UG inseparable from outside the
  renderer → practical ceiling = base-vs-DLC gating (show OW+UG together).
- **INPUT PATH mapped (2026-06-20, scripts find_input/2/3, doc `docs/windows_input_path_re.md`):** keyboard
  GetKeyState/GetKeyboardState → FUN_14074f880(modifiers)/fe00/FUN_14074f960(main poll). Gamepad XInputGetState
  → FUN_141f29010 (per-pad, live XINPUT_STATE at padObj+0x48 from table DAT_1430b92e0 stride 0x10) → FUN_141f6bad0.
  DirectInput8Create fallback. **CSPcKeyConfig singleton = DAT_143d5deb8** (FUN_1402438f0→FUN_1402429d0), KeyAssign
  ParamTypeA/B/C tables. The command→map-toggle edge is factory-dispatched + VMProtect (not statically traceable).
  Handle = watch live input (padObj+0x48 / XInputGetState / GetAsyncKeyState) for the toggle key + shadow flag.
- **★ OW/UG SOLVED — readable layer field found (2026-06-20, scripts find_getter/viewmodel/vm2/vm3/buttonlabel).**
  Via the 3 map-switch buttons → `CS::WorldMapSwitchDialog` + `CS::WorldMapViewModel` (vtable 0x142ad82e0, ctor
  FUN_1408855b0 = the cfc3 writer, 0x450). The LAYER SETTER = **`FUN_1409c40f0(dialog, target_layer)`**:
  writes `*(char*)(*(longlong*)(dialog+0x2B68)+0xB8) = target_layer` and applies `FUN_1409c8120(dialog, page + (layer?1:0))`
  → **underground = page+1 internally** (why the stored page 0xA88 stayed 0 for both OW and UG, and why cfc3
  global is transient/render-only). **THE GETTER/STATE FIELD = `*(uint8_t*)( *(void**)(dialog+0x2B68) + 0xB8 )`:
  0=surface(OW), !=0=underground(UG)** — a double-deref we never probed (snapshots only read flat dialog fields).
  Confirmed by the button-state fns: `FUN_1409d5ce0` (layer==0 → shows "Afficher souterrain"), `FUN_1409d5e40`
  (layer!=0 → shows "Afficher surface") — matches <user>'s inversion logic. `_IsChangeableOverlayLayer`
  (FUN_1409c6f70(dialog, page@0xA88)) = whether the toggle is available, not the state. **FULL GATING:
  page(0xA88)==10 → DLC ; else layer==0 → OW(60/61) ; else → UG(12).** dialog = cursor−0x2DB0. CT entry 30 +
  snapshot updated to read `[[MFG_MENU+2B68]+B8]`.

**★ VIEW-CENTRE / map-space→screen SOLVED — engine projection in plain floats (2026-06-20, scripts
`find_viewcentre`/`find_panwriter`/`find_panset`, doc `docs/windows_worldmap_viewcenter_re_findings.md`).**
Forward projection is EXACT: **`screen_local = marker·zoom − pan`** (pan=WorldMapArea `+0x378`x/`+0x37C`z in
1920×1080-canvas px, zoom=`+0x380`), then `×(realW/1920, realH/1080) + const bias`. **pan is device-independent**
— cursor tick `FUN_1409bd4b0` never writes it (only pan/zoom setters do). The 3 cursor coord pairs `+0xFC`/`+0x104`/
`+0x10C` are ALL the input-driven reticle (clamped to snap-rect vs full-rect `+0x350`=[0,0,10496,10496]) → none is a
centre (root of the markers-follow-cursor bug). **Rosetta = pan setter `FUN_1409cd100`(0x9cd100):** `panX = zoom·centreX −
(view+0x348++0x340)·0.5` (snap-rect centre = screen centre of view); combined setter `FUN_1409cd1c0`(0x9cd1c0);
inverse `FUN_1409cd0a0`(0x9cd0a0): `out=(in+pan)/zoom` maps screen→marker ⇒ invert = the forward above. **Brief's
failed attempt** used `centre=pan+(screen/2)/zoom` → `marker·zoom − pan·ZOOM` (pan wrongly ×zoom); fix = subtract pan
DIRECTLY (already screen-local px). If an explicit centre is wanted: `viewCentre=(pan+snapRectCentre)/zoom`. **SHIPPED:**
`project_uv` pan path rewritten to direct form, default ON, `Y` toggles old reticle centre (goblin_overlay.cpp).
Self-test (passes by construction): projecting reticle's own `+0x104/+0x108` lands on the mouse.
**RUNTIME UPDATE (2026-06-20, <user> tested):** (1) my first pan commit (839b07b) added a `×realW/1920`
canvas rescale = BUG (broke calibration). `screen=marker·zoom−pan` is algebraically IDENTICAL to the working
reticle baseline (`pan=reticle·zoom−screenCentre`) → SAME units, NO rescale. Removed in 09a87a7; default kept at
reticle until gamepad settled. (2) **GAMEPAD OPEN:** markers update on MOUSE motion only, NOT gamepad stick — the
menu-walk canonical cursor's reticle `+0x104/+0x108` doesn't move under stick. Static: tick FUN_1409bd4b0 DOES read
the stick (FUN_140757a10 @1409bd66a, gated by input-enable FUN_140758050); per-frame view updater FUN_1409c32f0
(←FUN_1409cfb60) drives pan setter toward target dialog+0x2eac/+0x2eb4 (NOT reticle directly). **Shipped runtime tool:
`input_delta_scan` in goblin_worldmap_probe.cpp** — logs every f32 on cursor (+0xE0..+0x160) & view (+0x340..+0x390)
that changes per tick (`[INPUT-DELTA]`). **RESULT (<user>, 2026-06-20): gamepad stick → view +0x378/+0x37C(pan)+0x380(zoom)
move, cursor +0xFC/+0x104 FROZEN; mouse → reticle + pan move.** So pan tracks both devices BUT <user> says pan is
UNUSABLE = **variant between game launches** (not reproducible) → pan-centre projection reverted to reticle default
(318c554 set pan default, 329091b reverted it). **Docs moved to `docs/re/`** (329091b). **DECISION (<user>): RE the
GAMEPAD cursor** — the menu-walk cursor is mouse-only; per-frame view updater FUN_1409c32f0 centres pan on dialog+0x2eac
(== cursor+0xFC, the mouse reticle), so gamepad pans via a path bypassing this reticle = a DIFFERENT cursor object.
**Shipped finder `scan_all_cursor_instances` (commit 0c3b487):** enumerates EVERY WorldMapCursorControl instance in
committed private RW mem (bounded VirtualQuery + chunked RPM, skips image + >256MB regions; NOT the O(GB) scan that
crashed); runs once per map-open; loop logs `[ALLCURSOR-MOVE] @addr` for any instance whose +0xFC/+0x104 changes,
tags menu-walk one (MOUSE). If nothing moves, gamepad reticle is a different class → trace
FUN_140757a10 consumer. (Static: only ONE WorldMapCursorControl ctor site = dialog+0x2DB0.)
**★ RESOLVED — device-independent centre = `(pan + snapMid)/zoom` (commit c403f35, 2026-06-20).** <user>'s clue: the
projection works at ANY zoom under the MOUSE, only desyncs under gamepad → not a separate object, the reticle is just
the wrong centre. Engine pan setter FUN_1409cd100: `pan = zoom·viewCentre − snapMid` (snapMid = midpoint of view
+0x340..+0x34c snap rect) → invert → **viewCentre = (pan+snapMid)/zoom**. Under mouse == reticle (drop-in for the
known-good baseline); under gamepad reticle frozen but pan updates ([INPUT-DELTA] confirmed) → tracks the stick.
**snapMid was the missing per-page term** that made bare `pan` look "instance-variant" (centre off by snapMid/zoom,
which changes per page). All 3 terms on the ONE deterministic WorldMapArea → NO separate gamepad cursor needed.
Shipped: `LiveView.snapMidX/snapMidZ` (view+0x340..+0x34c), `project_uv` centre=(pan+snapMid)/zoom in reticle-form
(default ON, Y=raw reticle A/B). All-instance scan demoted to opt-in fallback (env `MFG_GAMEPAD_CURSOR_SCAN`).
**Runtime-verify pending (<user>):** mouse → cyan ring on mouse (== reticle); gamepad → markers track the stick across zoom.

**★ GAMEPAD "map drifts / jamais centré" ROOT CAUSE (2026-06-20).** ER pans the 2D world map on the
**`(GetCursorPos − screenCentre)` delta** (cursor position = pan velocity; ER world-map cursor follows the
OS mouse via GetCursorPos, a separate path from the 3D camera's DirectInput). The mod's `hk_get_cursor_pos`
(goblin_overlay.cpp) returned screenCentre to the game ONLY when the F1 panel was open (`g_show`). So with the
map open WITHOUT F1, on **gamepad** the OS cursor is frozen OFF-centre → constant non-zero delta → **map drifts
forever**; the user's workaround was physically moving the mouse back to centre (delta 0). Confirmed user symptom:
"quand je bouge le joystick la map bouge, workaround move mouse, gaté sur mouvement souris." **FIX (deployed):**
freeze GetCursorPos→centre also when **gamepad connected + world_map_open() + mouse idle (>120ms)** (XInput
loaded dynamically, polled 1/s → no mouse-only regression: a mouse player's idle off-centre cursor still pans).
Gamepad stick pans via its own path (tick FUN_1409bd4b0 reads analog FUN_140757a10), NOT GetCursorPos, so it
still works. This is the GAME's map drift — separate from the overlay MARKER projection (reticle centre, default;
the (pan+snapMid)/zoom view-centre is behind the Y toggle, "stayed broken" per user 2026-06-20).

**Live READ confirmed (2026-06-18, app 2.6.2.0, ERR 2.2.9.6, 720p windowed):** with the world map OPEN +
cursor moving, the probe captured the active cursor — **`+0xFC` = X (marker space, ranged 3183→3843), `+0x104`
ranged 2472→4057** (so RVA 0x2b29a90 is correct, no drift; the earlier all-zero log was just map-not-open).
⚠️ Probe pitfall: it only logs instances whose coords changed — **map must be open + cursor moving** or you
get only dormant ≈0 cursor objects. Per decompiler the tick re-writes the pair **`(+0xFC, +0x100)`** (line 245)
= true reticle pos; probe logged +0x104 (a neighbour field). **Still TODO (write test, Cheat Engine):** confirm
which Z partner (+0x100 vs +0x104) actually moves the reticle when written + that the view pans.

**★ MID-SESSION RESOLUTION SWAPCHAIN CORRUPTION — static side RE'd, runtime decisive (2026-06-21,
scripts `find_screenmode.java`..`find_screenmode4`, `find_resize`/`find_resize2`, `find_rendcfg`/
`find_rendcfg2`, `find_applyres`; doc `docs/re/windows_midsession_resolution_swapchain_re_findings.md`;
brief from commit 5b8c26b).** Entry = <user>'s clue, the **"Screen Mode"** string → `CSScreenModeCtrl`
(FD4Step `CSStepLocal<CSScreenModeCtrl>`, vtable found via RTTI @0x143d0b150; apply logic in its
step-method table `this[1]`, not vtable). **Hypothesis 1 (flat stale W/H global in map math) REFUTED:**
map fns `FUN_1409bdc50`(pan)/`FUN_1409ce190`(pan-bounds) read only CONSTANTS — `1920`=`DAT_14329e6f8`,
`1080`=`DAT_14329e6f4`, scale `0.5`=`DAT_14329e660`, LUT `FUN_1409e67e0`=`DAT_142b324b0[]` static. The
ONLY res-dependent input = **GFx/Scaleform stage viewport rect** `obj+0xe8/0xea/0xec/0xee` (ushort
x/y/w/h) from `FUN_140d2e710`→`FUN_140d4cc50(DAT_143d81ee8 gfxResMgr, 0x8f, 0)`. Input symptom (FD4 GUI
hit-test `FUN_140d7ff40`) = same Scaleform layer ⇒ **both symptoms = one stale GFx stage/GUI viewport**;
3D fine (rides render-output targets that DO refresh). **Display subsystem `0x140e8xxxx`:** window-mgr
singleton `DAT_1445894f8`, render/display-mgr `DAT_1447ef360`(=proj-doc `+0x47ef360`). Screen-mode config
struct: mode`+0xAC`(0win/1full/2borderless), W`+0xC0`, H`+0xC4`, descriptor base`+0xC0`(~0x24B), display`+0xD4`.
`FUN_140e8f910`=res-per-mode (fallback `1920×1080`). **★ Per-frame apply driver `FUN_140e898d0`** (AOB
`40 57 48 83 EC 20 80 B9 B9 00 00 00 00 48 8B F9`) calls **`FUN_1419eac90(DAT_1447ef360,0,cfg+0xC0,0)`** (AOB
`48 8B 81 28 01 00 00 4C 8B D9 4C 8B 91 30 01 00`) = **DEFERRED-apply stager**: walks render-output list
`mgr+0x128..+0x130` stride `0x170` key`entry+0x128`, stages res→`entry+0x144..+0x164`, sets dirty
`entry+0x140=1` + `mgr+0xeb8=1` → render thread re-derives next tick (swapchain resize = VMP/render-thread,
not static-traceable). **★ CORRECTED ROOT CAUSE (<user> clue: the 3D WORLD is ALSO zoomed, not just the map — both 16:9 so it's a
pure ~1.5× scale error = oldW/newW).** GFx-stage-only theory REFUTED. The stale value = **ACTIVE render-target
dims `[DAT_1447ef360+0x128 + outIdx*0x170]+0x118(W float)/+0x11c(H float)`** — read by BOTH the 3D viewport AND
the map fit `FUN_140d84990` (`fVar1=*(entry+0x118)`). Computed by **`FUN_1419ebb40(mgr,outIdx)`** (writes
entry+0x108/0x10c int, +0x110/0x114 offsets, +0x118/0x11c float) from source dims `mgr+outIdx*0x30+0x3c8/0x3cc`
+ render-scale`+0x3ec`; `outIdx=-1`=all. Dirty consumer = **`FUN_1419ed870`** (`if(mgr+0xeb8){...}`, only
re-derives entries with `+0x140` set); full re-apply = **`FUN_1419ed440(mgr,W,H)`**. **Bug:** mid-session resize
marks only the primary swapchain output dirty → other render-output entries (3D view / UI-map) keep stale +0x118
→ 1.5× zoom everywhere. **FIX (doc §5, ranked): (1) `FUN_1419ebb40(DAT_1447ef360,0xffffffff)`** from hk_resize_buffers
= recompute active dims for ALL outputs, NO swapchain side-effects, safest (AOB `48 8B C4 55 41 54 41 55 41 56 41 57
48 8D A8 B8`); (2) `FUN_1419ed440(mgr,w,h)` full re-apply (heavier, AOB `40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05
5D`); (3) raw-poke +0x118/+0x11c. renderMgr=`*(void**)(er_base+0x47ef360)`. **RUNTIME-DECISIVE (<user>, §5.4):**
dump every output entry `+0x118/+0x11c`+`+0x140` before/after 1080→720, find the un-refreshed one; then test fix (1).
C++ sketch doc §5.3. Scripts +`find_zoomfit/find_rendims/find_promote/find_reapply`. See [[workflow-preferences]].

**★ RUNTIME WORLD→MAPSPACE PROJECTION — fully RE'd, "kill baked LegacyConv" brief ANSWERED (2026-06-22,
scripts re_v62..v65, doc `docs/re/windows_world_to_mapspace_projection_re_findings.md`).** The native map
projects EVERY point via an array of **converter entries inline in `CS::WorldMapViewModel`** (vtable
`0x142ad82e0`): array `VM+0xF8` stride **0x30**, count `VM+0x280` (ctor sets 8). Projection fn =
**`FUN_140876140`** (0x876140) per entry: if `conv+0x28`(legacy node)≠0 it calls **`FUN_1408775e0`**
(0x8775e0) to remap packed id + TRANSLATE world by the conv base (RB-tree @ node+0x10, keyed by packed id) →
**the engine folds WorldMapLegacyConvParam LIVE** (drop baked LEGACY_CONV). Then if `(id>>24)==conv+0xB`(area):
`mapX=(posX+(gridX−conv+0xA)·256 − originX(+0xC))·scale(+0x20)+biasX(+0x18)`, `mapZ=−(...Z...)·scale+biasZ(+0x1C)`.
⚠️ world arg = **area-LOCAL pos** {posX,posY,posZ}, NOT gridX·256+pos (fn adds the grid term itself). Converter
fields pinned by builder **`FUN_140876100`**: +0x08 key(+9 gridZbase,+0xA gridXbase,+0xB area), +0xC originX,
+0x14 originZ, +0x18 biasX, +0x1C biasZ, +0x20 scale, +0x28 legacyConvNode. Built in VM ctor **`FUN_1408855b0`**
(0x8855b0, writes vtable) from `CS::WorldMapLegacyConvParamGroup` (regulation-driven → self-adapts to ERR/mods).
**Callable entry = `FUN_1408877d0`** (0x8877d0): `(VM, Vec2* outMapXZ, u32* packedId, Vec3* worldLocal)` loops
converters, returns 1+fills map-space. **Page = matched-slot index into byte table `0x142ad82f8` = `[00 01 0a]`**
(slot0→0 OW, slot1→1 UG, slot2→10 DLC; guard i<3), + override area==0x0c(12)⇒UG (in `FUN_140886b10`/`FUN_140887870`).
→ drop `marker_group_from`. **m19 Chapel** (0 conv rows): no converter accepts → fn returns 0 → game doesn't place
it → GATE. **DLC origin/bias not baked anymore** — it's just slot2 in the live array when the DLC map is open
(read it). **Brief leads were stale**: FUN_140d82770 = stage-2 Scaleform fit (mapspace→screen), NOT this; build/
reconcile FUN_140a82a80/a832a0 carry no projection math. **Runtime-confirm pending** (game not running during RE):
dump live array per page for DLC/UG slot2 constants + slot↔area↔page; validate a known grace via FUN_1408877d0.
Both ghidra-proj2 + CT `MapForGoblins_converter_dump.CT`. See [[workflow-preferences]].

**★ RUNTIME ICON TEXTURES — Scaleform→D3D bridge RE'd, "kill FFDEC atlas" brief (2026-06-22, scripts
re_v66..v70, doc `docs/re/windows_runtime_icon_textures_re_findings.md`).** Goal: read the game's own
worldmap icon textures live instead of the baked FFDEC PNG atlas. **Bridge:** GFx worldmap movie image
(built by `CS::CSScaleformImageCreator`, create-img vt[1] `FUN_140d6bbc0` 0xd6bbc0) → **`CS::CSTextureImage`**
(`Scaleform::Render::TextureImage` subclass, ctor `FUN_140d68410` 0xd68410) → **`CS::CSGxTexture`** (0x2b761b0)
→ **`GXBS::GXTexture2D`** (0x2f05928, ctor `FUN_1419efa90`) → **ID3D12Resource**. ★ The per-sprite UV RECT is
a PLAIN C++ FIELD on CSTextureImage (not just in the Flash timeline): rect x0/x1 `+0x74/+0x7c`, y0/y1
`+0x3c/+0x40`, full-sheet W/H `+0x2c/+0x30`, backing tex ptr `+0x38`. GXTexture2D fields: **resource `+0x40`**
(D3D desc at [+0x40]+0x20; getters vt6/7/8/9/15=W/H/format, vt16 IsValid=+0x40!=0), SRV `+0x48`, format `+0x58`.
**iconId side:** point build `FUN_140a82a80` (already hooked) does NOT pick the sprite — Scaleform/VMP does;
iconId read via `CS::WorldMapPointPinData::GetIconId` (RTTI 0x2ad6704), bound on `CS::WorldMapItemControl` GFx
item. So iconId↔CSTextureImage map = RUNTIME capture (hook CreateImage 0xd6bbc0 on menu load, log rect+tex). 
**Path B native** (`CS::CSMovieGxTexture` 0x2bded88 ctor `FUN_140e1f5a0` renders a movie→GX tex via display
mgr `DAT_1447ef360`+`FUN_1419e7990`) but VMP-fragile; **VERDICT Path A** = CopyTextureRegion the sub-rect from
the engine ID3D12Resource into our own SRV texture (GPU→GPU, ImGui samples BCn fine, no CPU de-pitch/de-BCn),
using captured g_device/g_command_queue/g_srv_heap. Grab during worldmap-open frame (resource in
PIXEL_SHADER_RESOURCE); don't mutate engine state. GX texture repo = `CS::CSGxResourceRepository` (ctor
`FUN_140b54260` 0xb54260); TPF loader `GXBS::GXCGTextureBuilder_TPF` 0x2f0a228. Builds on the just-landed live
projection (same read-live-not-bake). RUNTIME-UNVERIFIED (game not running during RE). See [[workflow-preferences]].

**★ PAGE-TRANSITION animation state RE'd — no clean progress struct, brief answered (2026-06-22, scripts
re_v71..v73, doc `docs/re/windows_worldmap_page_transition_re_findings.md`).** Goal: sync overlay markers to
the native page-swap (OW⇄UG⇄DLC) cross-fade/pan instead of popping. **Finding: ER keeps NO {transitioning,
0→1 progress, from, to} struct.** What's real: (1) page id flips INSTANTLY at input — switch handlers
`FUN_1409c40f0`(layer 0x9c40f0)/`FUN_1409c5d20`/`FUN_1409c7900`/`FUN_1409c1fc0` write `dialog+0xA88` + 9 list
page fields (`FUN_1409c8120` 0x9c8120, lists @ dialog+0x30DC..+0x389C stride ~0xF8) + set `dialog+0xA44=1` +
play SE (`FUN_140814ed0`, the 400=sound id NOT a duration) + reset cursor snap. (2) **`dialog+0xA44/0xA45/0xA46`
are PER-FRAME TRANSIENT** — set then CLEARED at end of every per-frame step `FUN_1409c32f0`(0x9c32f0, `MOV word
[+0xA44],0`); so +0xA44 = swap-EDGE (detect), not a sustained flag. (3) Real multi-frame anim = **view pan/zoom
EASE toward target `dialog+0x2EAC`(pan vec2)/`+0x2EB4`(zoom)** (cur = view `cursor+0xF0`+0x378/+0x380) + two
dt-countdowns **`dialog+0xE00`(f32 ~0.2s, its SIGN also picks surface/UG = the cross-fade interpolant)** and
**`dialog+0x3E74`(f32)**; predicate **`FUN_1409cd020(view)` 0x9cd020 = "view animating"**. (4) Surface↔UG
cross-fade alpha is SCALEFORM-side (per-list vt[0x38] fed +0xA44; `FUN_1409c38d0` 0x9c38d0 drives it) — no flat
float. (5) FROM page not stored (overlay must remember prev); TO = dialog+0xA88. **Overlay recipe:** detect swap
via page-change/+0xA44 edge → snapshot fromGrp + startDist; transitioning = view≠target || +0x3E74>0;
progress = 1−dist(cur,target)/startDist (move) or +0xE00/0.2 (fade); draw fromGrp@(1-p) + toGrp@p, both from LIVE
view (positions already ride pan/zoom since projected live → bug is set-membership+alpha, not position). Builds
on the live projection. RUNTIME-UNVERIFIED. See [[workflow-preferences]].

**★ LEGACY-CONV LIVE FOLD — replicate map-CLOSED from WorldMapLegacyConvParam, brief answered (2026-06-22,
scripts re_v64+re_v74, doc `docs/re/windows_legacyconv_param_live_re_findings.md`).** Goal: minimap (map-CLOSED,
no VM) folds legacy dungeons→overworld from the live param instead of baked LEGACY_CONV. **Paramdef
WORLD_MAP_LEGACY_CONV_PARAM_ST (row 0x30):** srcArea +0x04, srcGX +0x05, srcGZ +0x06, srcPosX +0x08/Y+0xC/Z+0x10,
dstArea +0x14, dstGX +0x15, dstGZ +0x16, dstPosX +0x18/Y+0x1C/Z+0x20, isBasePoint +0x24. **Live fetch = EXISTING
infra:** `from::params::get_param<WORLD_MAP_LEGACY_CONV_PARAM_ST>(L"WorldMapLegacyConvParam")` (same as
WorldMapPointParam; SOLO_PARAM_LIST AOB) — resident MAP-CLOSED. **Fold = world-space translate:** dstWorld =
markerWorld + (dstBaseWorld − srcBaseWorld) of matched row; world = grid·256+pos. **3 corrections vs the data:**
(1) **MULTI-HOP** — param chains (m35→11→60); engine FLATTENS at VM-build (`FUN_1408776e0` 0x8776e0 inserts
composed nodes; node +0x1c key, +0x20 dstId, +0x24/+0x28/+0x2c f32 world delta), so map-closed we must COMPOSE
the chain (the mod's Python `generate_legacy_conv_cpp::resolve()` already does). (2) **identity/terminal =
area∈[50,88]** (`FUN_140660fe0` 0x660fe0: `(area-0x32)<0x27`) NOT just {60,61} — sources are areas<50 (legacy
10-43, base-UG 12); baker's {60,61} test drops chains ending at other 50-88 areas. (3) **key on FULL block
(area,gx,gz)** packed area<<24|gx<<16|gz<<8 (engine tree key) NOT (area,gx) — the latter = mod's area-16
wrong-region bug. Renormalize `FUN_140877840` 0x877840 (carry |pos|>128 into grid) is automatic in world space.
Plan: add struct to from::paramdef, replace project_dungeon_row_to_overworld + baked LEGACY_CONV (all generated*)
+ dlc_ug_eyeball with live fold (pre-flatten on load); keep VM `worldmap_probe::project` as map-open fast path.
RUNTIME-UNVERIFIED. Builds on live projection [[ghidra-worldmap-re]]. See [[workflow-preferences]].

**★ ICON-TEXTURE FOLLOW-UP — lazy bind solved, live-validated (2026-06-22, scripts re_v75..v77, doc
`docs/re/windows_runtime_icon_textures_followup_re_findings.md`).** User built the CreateImage hook (probe
`dump_icon_textures`/[ICONTEX]) + RPM — validated CSTextureImage vt 0x2bb8910, but BFS found no GPU texture.
**Why: LAZY BIND.** `CSTextureImage+0x10` = bound `Scaleform::Render::Texture`, populated lazily on FIRST RENDER
by **GetTexture = vt[+0xA8] `FUN_140d607f0`** (0xd607f0; resolves via GFx renderer/texmgr singleton `DAT_144593250`,
create-fn `FUN_141147700`). At CreateImage time +0x10 is null → probe ran too early AND stopped one hop short
(+0x10 is a Render::Texture, GXTexture2D is one more hop). **FIX: read `img+0x10` on a MAP-OPEN frame** (or call
`img->vt[+0xA8](DAT_144593250)` on render thread) → Render::Texture → GXTexture2D (vt 0x2f05928) → +0x40
ID3D12Resource. **★ RECT OFFSETS CORRECTED (live):** contiguous `img+0x74`x0/`+0x78`y0/`+0x7c`x1/`+0x80`y1, dims
`+0x84/+0x88` (also +0x2c/+0x30) — my prior findings' +0x3c/+0x40 for y were WRONG (patched both docs). 66×66
sprites, sheets 512×512 / 2048×1024. **iconId↔image = NO flat C++ map** (iconId→sprite is GFx/VMP): label by the
image IMPORT NAME (CreateImage param_3 → DLString; resolve `FUN_140d64490` keys `L"%s_ptl"`) or a one-time on-screen
hover correlation. Sheet alt routes: GXCGTextureBuilder_TPF 0x2f0a228, CSGxResourceRepository 0x2b6eac8. Path A
(CopyTextureRegion rect → our SRV, GPU→GPU) viable. See [[workflow-preferences]].

**★ MENU ITEM-ICON pipeline RE'd — corrected EquipParam iconId offsets (2026-06-22, scripts re_v78..v80 +
paramoff.py, doc `docs/re/windows_menu_item_icon_re_findings.md`).** Pivot from worldmap icons (can't reach
the 6 MFG categories) to inventory icons (menu draws every item). **The probe's zero-match was an OFFSET BUG**
(hand-summed iconId offsets all +3 too high, Gem +1; bitfield mis-pack). ⚠️ **THE OFFSETS I GAVE HERE WERE ALSO
WRONG — SUPERSEDED.** I computed Weapon 0xBF/Protector 0xA7/0xA9/Accessory 0x27/Goods 0x31/Gem 0x05 from the repo's
PRE-DLC paramdef XML; the LIVE 2.6.2.0 build is **Weapon 0xBE, Protector iconIdM 0xA6/iconIdF 0xA8, Accessory 0x26,
Goods 0x30, Gem 0x04** (solved via SOTE Paramdex × live-distinct columns — see the live-paramdef + grace-pin
entries below). LESSON: read ER param offsets from the live build, never a bundled paramdef XML. **Format site:**
`FUN_14073d9d0`(0x73d9d0) picks
`fmt=(&PTR_MENU_ItemIcon_%05d_0x3b34c28)[iconRef.type]` (type 0=ItemIcon/1=PropertyIcon/2=StatusIcon) + vswprintf
`FUN_14013a1f0`(0x13a1f0); resolver caller `FUN_14074bcc0`(0x74bcc0). **icon-ref = {u8 type@+0, s32 id@+4}**, id =
item iconId. **MENU_FL = SEPARATE:** `MENU_FL_<digits>` = the gfx atlas image name per iconId (ERR high range, so
40147 IS an item iconId — confirm via corrected verify); `MENU_FL_Affinity_*`/`_Evaluation_*` = named CB_imgTag
glyphs in FMG `<img src='img://'>` text (resolver `FUN_140d1a7c0` 0xd1a7c0, tag table stride 0x28 name@+0x10). **#4
iconId→rect = GFx-ONLY** (no C++ table; rect = gfx sprite layout, surfaced per-draw on CSTextureImage +0x74.. —
capture at draw via the CreateImage hook, or FFDEC). Inventory DOES draw all categories → every iconId's rect is
capturable (unlike worldmap). Plan: item→iconId (corrected offsets) → CreateImage captures MENU_FL_<id>+rect on
the 2048×2048 atlas → resource via solved chain (img+0x10→+0x70→ID3D12Resource) → CopyTextureRegion. See
[[workflow-preferences]].

**★ LIVE PARAMDEF / iconId offset — NO queryable paramdef, self-calibrate (2026-06-22, scripts re_v81..v82,
doc `docs/re/windows_live_paramdef_offset_re_findings.md`).** My earlier item-icon doc gave Weapon iconId=0xBF
from the repo's VANILLA paramdef XML — WRONG: live build is **0xC0** (the repo XML is PRE-DLC, ~1 byte short;
0xBF is odd, 0xC0 even). **Verdict ask#1 = NO live paramdef:** `EQUIP_PARAM_WEAPON_ST`/`EquipParamWeapon`/`PARAMDEF`/
`ParamdefMetaData` have ZERO exe xrefs — type strings + names live ONLY in loaded regulation data; exe accesses
fields by COMPILED offsets (mod's get_param-by-name just walks the loaded list). So offset is BUILD-FIXED (not
runtime-drifting); no name→offset map to query. **Best route = SELF-CALIBRATION** (zero offline, patch+mod proof):
at startup scan candidate byte offsets per EquipParam, pick the u16 column that's sane (1..~70000, non-constant,
matches a known anchor item's iconId) — exactly how 0xC0 was found. Alt: ship 2.x/SOTE Paramdex paramdefs (then
the calculator is correct). **#3 no offset-free engine call:** iconRef {u8 type@+0, s32 id@+4} consumed by widget
`FUN_14074bcc0`(0x74bcc0)→`FUN_14073d9d0`; the id is set by inventory populator reading the compiled +0xC0 — any
"engine call" still bottoms out at +0xC0, so self-calib is the cleanest equivalent. LESSON: read param offsets
from the DECOMP/live build, NOT the repo's bundled (versioned) paramdef XML. See [[workflow-preferences]].

**★ ITEM→EquipParam DISPATCH + iconId offset method (2026-06-22, scripts re_v83..v87, doc
`docs/re/windows_menu_cursor_iconid_populator_re_findings.md`).** Both my prior iconId offsets WRONG (0xBF
pre-DLC XML; 0xC0 = [CALIB] density distinct=2, not iconId). Found the **item→EquipParam-row dispatch**:
item-handle→id `FUN_14073d600`(0x73d600); **category = id>>0x1C** (0=weapon,1=protector,2=accessory,4=goods,
8=spirit-ash), rowId = id&0x0FFFFFFF. **Per-category ROW RESOLVERS** (binary-search the param table
`*(*(x+0x80)+0x80)`, stash row ptr @ wrapper+0x8): weapon **FUN_140d54600**, protector **FUN_140d47460**,
accessory **FUN_140d21eb0**, goods **FUN_140d39df0**, ash FUN_140d2a360. These are clean hook/call points
(= the mod's get_param row search). **Did NOT isolate the single movzx [row+0xXX] iconId instruction** — it's
1 of ~dozens of near-identical 0x674xxx/0x675xxx property getters (text via DAT_143d7d4f8+FUN_140d10xxx;
numeric direct reads e.g. FUN_140674680 reads row+0x3a). **AUTHORITATIVE OFFSET METHOD = resolve+confirm:** for
a KNOWN item whose iconId is captured live ([ICONMAP] MENU_FL_<N>), scan its resolved row's u16 offsets for the
one == N, require ≥2 anchors/category (per-item deterministic, kills the 1-anchor statistical ambiguity). Goods
candidate 0x20 — confirm with a 2nd goods item. **iconId→rect = GFx-only** (confirmed; C++ ends at the iconId
VALUE; rect = gfx sprite, CreateImage capture/FFDEC). LESSON REINFORCED: ER param offsets need live/decomp
confirmation, never a byte-sum or single coincidental anchor. See [[workflow-preferences]].

**★ RUNTIME ITEM-ICON SPRITE — draw-free symbol→image find, residency-limited (2026-06-22, scripts re_v88..v90,
doc `docs/re/windows_runtime_item_sprite_re_findings.md`).** Goal: sprite rect+pixels for an ARBITRARY iconId
from the LOADED gfx (mod-robust, no FFDEC), past the "CreateImage only fires for drawn icons" wall. **Icons
grouped into SHEETS of 1000:** per-icon fmt `MENU_ItemIcon_%05d` (table 0x3b34c28), per-sheet
`MENU_ItemIcon_%02d000d` fed iconId/1000 (table 0x3b34c40, via FUN_14073d5a0) → **sheet=iconId/1000,
cell=iconId%1000**; ERR per-icon symbol = MENU_FL_<id>. **DRAW-FREE find-by-name:** `FUN_140d63c30`(0xd63c30,
twin FUN_140d63e50) (repo `DAT_143d82510` FD4 image singleton, &out, L"MENU_ItemIcon_<id>") → CS::CSTextureImage
(rect +0x74/78/7c/80) or 0 on miss; widget FUN_14074bcc0 does per-icon key→miss→sheet-key fallback. Finds any
LOADED-sheet icon, NOT just drawn ones. **Movie resource WALK:** FUN_140d69640(movie) iterates [movie+0x40]+0x90,
type +0x88==4=image (caller FUN_140d790a0) — enumerable. **#2 sheet resource:** found img → solved chain
img+0x10→Render::Texture(0x2c0f318)+0x70→ID3D12Resource, draw-free. **#3 force-create:** FUN_140d6bbc0 (needs
GFxMovieView/menu loaded, render-thread; fragile). **#4 mod-robust:** symbol→rect is in the LOADED gfx (only the
fmt strings are exe-baked) → auto-adapts to ERR/Convergence/ERTE. **★ RESIDENCY LIMIT (real caveat):** item-icon
sheet loads with the inventory menu, likely EVICTED when closed → world-overlay loot markers (menu closed) can't
find → must CAPTURE-WHILE-OPEN into our own atlas, keep-resident, or force-load. Decompiler noreturn-misflag on
FUN_14011b190 truncated the exact find return (find-vs-load); runtime-confirm: call FUN_140d63c30 for a
non-displayed item w/ menu open. iconId offsets now SOLVED upstream (Weapon 0xBE/Protector 0xA6/0xA8/Accessory
0x26/Goods 0x30/Gem 0x04 via SOTE Paramdex × live-distinct — my recommended method). **P2b SHIPPED** live
(find-hook harvest→CopyTextureRegion BC1→our SRV→drawn; sheets BC1_UNORM fmt 71, 4096×2048; GPU copy persists
→ eviction moot; find-by-name CRASHES on non-resident = my residency caveat confirmed). Limit became COVERAGE
(browse-to-fill). **★ Option-3 WALK-ALL container SOLVED (re_v91, disasm 0xd69662..0xd696f5):** movie+0x40→res
(gate res+0x88==4=image-list), res+0x90→list; list+0x78 u32 count, list+0x80→arr (entry-ptr array stride 8);
entry+0x18 = name DLWString (heap iff *(entry+0x30)>=8, else inline), entry+0x8 refcount. Walk it to harvest ALL
resident icons per menu-open (vs the per-find hook's one); names resident→resolve is SAFE. NB the list is a
DOUBLE-deref [movie+0x40]+0x90 NOT movie+0x90 (earlier [p1+0x90] dump missed the +0x40 hop). Movie ptr captured
via the ENUM hook on FUN_140d69640. See [[workflow-preferences]].

**★ NATIVE GRACE-PIN system = WorldMapWarpPinData, NOT +0x398 (2026-06-22, scripts re_v92..v95, doc
`docs/re/windows_native_grace_pin_manager_re_findings.md`).** +0x398 CSWorldMapPointIns map proven EMPTY in
overlay-sole mode → graces are a SEPARATE system: sites-of-grace = bonfire WARPS. **Classes:** CS::WorldMapWarpData
(vt 0x2ad8840, 0x38B item) in CS::WorldMapWarpDataList (vt 0x2ad8898, MenuViewItemList vector begin/end/cap @+1/+2/
+3 stride 0x38); drawn pin = CS::WorldMapWarpPinData (vt 0x2ad8228) in CS::WorldMapPinDataList<WarpPinData> (vt
0x2ad82a8). Hangs off WorldMapViewModel/dialog (the WarpList, 1 of the 9 WorldMapItemControl lists). **Discovered
GATE:** WarpData+0x8 = source bonfire entry; **state/discovered = [warpData+8]+0x1E byte bits 0/1/2**
(FUN_140d25540 0xd25540), **iconId = [warpData+8]+0x08** (FUN_140d25650 0xd25650), id @+0x18. **Build:** FUN_14088b060
(0x88b060, src FUN_14088b3e0 → copy-ctor FUN_14088a0f0 0x88a0f0 → WarpDataList); WarpPinData builder FUN_14088b7b0
(0x88b7b0, WarpData→pin, reads state+iconId); setup FUN_14088a6c0/FUN_14088aba0. **Suppression (grace-only, keeps
player-dot/fog/objectives since separate list):** hook FUN_14088b7b0 to skip discovered, or clear the
PinDataList<WarpPinData> via its own method — NOT hand-walk (the [PINSET] crash). **#4:** [er+0x3D6F558] & player-pos
[er+0x3D69BA8] are point/player subsystems = KEEP. **§6 sprite:** SB_ERR_Grace_Morning_Color (ERR gfx, live-captured;
not in exe) in the WORLD-MAP movie image-list → §8-walkable (hook FUN_140d69640 on the world-map movie vs inventory).
**RUNTIME (theirs):** pin the live WarpPinData list via VM/dialog scan (size-N vector stride 0x38) or
find-what-accesses on FUN_140d25540; point dump_native_pins at it. See [[workflow-preferences]].

**★ GAMEPAD "active input device" FLAG — static side ruled out, runtime CE needed (2026-06-20,
scripts find_actdev/find_actdev2, out_actdev*.txt, doc `docs/re/windows_gamepad_input_device_re_findings.md`).**
Goal: the flag a REAL mouse MOVE sets that fixes the gamepad world-map (injected input can't trigger it).
Method = instruction-level data-xref INTERSECTION of the mouse-poll path (FUN_140e1e940/e1e500/e33aa0)
vs gamepad-poll path (FUN_141f29010/f6bad0/f2a4e0/f28260). **Intersection = only `__security_cookie`
(DAT_143c5adb0) + `_tls_index`** = pure noise ⇒ **the device flag is NOT a flat global and NOT in the
GetCursorPos/XInput poll paths.** Mouse poller writes only OBJECT fields param_1+0x30(moved)/+0x31(in-win),
reads singleton-guard DAT_14458b890. The "Keyboard"/"GamePad"/"GamePadAnalogEvent" strings = **Scaleform
(GFx) AS3 event bridge** (FUN_14106b890/bd80/be80) + controller-focus mgr (FUN_140f491d0) — decompiled,
ALL have empty writable-global-WRITE sets (mutate object fields, string-keyed dispatch). ⇒ device state is
**object-relative inside an input-manager singleton (candidate `CSPcKeyConfig` = DAT_143d5deb8)**, flipped
by the **raw-input/HID path which is VMProtect'd / dynamically resolved (GetRawInputData not name-resolvable,
find_rawmouse empty).** **VERDICT: not statically reachable → DECISIVE step = runtime CE memory-diff** (the
brief's "FASTEST path": gamepad-open=A, real-mouse-move=B, Changed/Unchanged narrowing → address + A/B +
"what writes" = the raw-input setter + pointer path, likely DAT_143d5deb8+0xNN). Then bake a guarded WRITE
of "mouse" in goblin_worldmap_probe.cpp gated on map-open + XInput-connected (memory writes aren't filtered
like injected input). Blocked on <user>'s live CE capture. See [[workflow-preferences]].

**★ RESIDENT-ICON FULL ENUMERATION — SOLVED via the FD4 image-repo tree walk (2026-06-22, scripts
re_v96/re_v97, doc `docs/re/windows_resident_icon_enumeration_re_findings.md`, CT
`tools/cheat_engine/MapForGoblins_icon_repo.CT`, RPM `<ghidra_scripts>\walk_icon_repo.py`).**
Supersedes the §8 movie-walk (which was load-screen-scoped on this build). The find-by-name
`FUN_140d63c30(repo,&out,key)` arg0 repo = static singleton **`DAT_143d82510` (er+0x3d82510)**, an
`FD4ResRep`/`FD4ResCapHolder` whose by-name lookup is a plain **MSVC `std::map<DLWString,
CSTextureImage*>` at `repo+0x80`** (lookup `FUN_140d62c10`; twin `FUN_140d63e50` = 2nd map at
`repo+0xb0`). **WALK RECIPE:** repo=*(er+0x3d82510); _Myhead=*(repo+0x88) (==end); _Mysize=*(repo+0x90);
root=*(_Myhead+0x08). Node = MSVC rb-node: `_Left+0x00 _Parent+0x08 _Right+0x10 _Isnil(u8)+0x19`; key
DLWString `+0x28` (len`+0x38`, cap`+0x40`, heap-ptr@+0x28 iff cap>=8 else inline); **value
CSTextureImage* directly at `node+0x50` — NO re-resolve-by-name.** DFS skip _Isnil!=0; filter key by
`MENU_ItemIcon_` prefix → iconId=atoi(name+14); img guard vtable er+0x2bb8910, rect img+0x74..0x80,
sheet img+0x10→+0x70→ID3D12Resource, fmt sheet+0x30. `FUN_140d69640` confirmed = per-movie find-one
(load-screen only here), superseded. Still RESIDENT-only (force-load = secondary lead, create path
FUN_140d5fee0/d60d00/d622f0 is in-repo register, not a TPF loader). **VALIDATED LIVE 2026-06-22 via the
CT: repo _Mysize=938 images, 163 MENU_ItemIcon_*, all vt=OK, rects+sheets correct.** ⚠️ DXGI format is
NOT at sheet+0x30 (reads 0 live; in-DLL cache_icon_from_img uses the same wrong offset — goblin_inject
.cpp:2349 TODO confirms format offset unidentified) → separate open sub-task: probe sheet+0x00..0x100 for
the W/H/format triple. DONE: in-DLL `harvest_repo_icons()` walk added (goblin_inject.cpp, commit
f8a0d4a, triggered from find_detour, _Mysize-throttled). ⚠️ But it's RESIDENT-ONLY = "tout du menu,
pas tout du jeu". **FORCE-LOAD investigated re_v98–v100: NO clean runtime lever** — find/widget
(FUN_14074bcc0) never loads (draws nothing on miss); CreateImage FUN_140d6bbc0 only builds a per-symbol
VIEW (`%s_ptl`) into an already-resident atlas + needs a live GFxMovieView; atlas TPFs are streamed by
the menu orchestrator FUN_140d790a0 (menu/movie ctx + FD4 task system, fragile); "RequestReloadMenuTexture"
= dead command string (no handler). Repo only gains MENU_ItemIcon_<id> after GFx binds the movie (menu
init). ⇒ 100% coverage = OFFLINE extraction (tools/extract_subtextures.py crops sblytbnd+DDS, vanilla+ERR)
+ keep repo-walk as live/mod override. User asked for force-load TWICE (incl. "tente quand même");
re_v101 confirmed FUN_140d790a0 is a per-frame TICK (ticked by FUN_140d724c0/FUN_140d78060) draining a
menu-open-fed load queue (manager+0x1df0/+0x1e08) — no bounded by-name load entry; force-load = locate
live manager singleton + build movie-load requests + drain via FD4 task system = re-implementing menu
init (crash-prone). Concluded NOT worth shipping; recommended offline extraction. See [[workflow-preferences]].

**Grace warp-pin DRAW-GATE — SOLVED (2026-06-22, re_v119..v121).** Discovered-grace pins =
`WorldMapWarpPinData` (vtable `0x2ad8228`, 0x350B), built by `FUN_14088b7b0` `0x88b7b0` (hooked),
value-copied (`FUN_140885ed0`→`FUN_140885500`) into the VM warp list at `WorldMapViewModel+0x2d8`
(VM ctor `FUN_1408855b0`, list vtable `0x2ad82a8`, stride 0x350). **Draw-gate = byte `pin+0xC`
(cached VISIBLE flag) but it is RECOMPUTED every map update by `vt[3] FUN_14087afa0` `0x87afa0`
from the per-layer STATE bitmask at `pin+0x60`** (`vis &= (state>>layerbit)&1`; layerbit via
`FUN_140887e90`). ⚠ The old "phase B" hook zeroed `pin+0xC` (the engine's scratch OUTPUT) → next
tick recomputed it back → "failed". ROOT cause = Ghidra offset misread: `param_1` is `undefined8*`
so its `param_1+0xc` = **byte 0x60**, not 0xC. **FIX (shipped): in `warp_pin_detour` zero the u32
at `pin+0x60` (state) instead of `pin+0xC`** → vt[3] yields invisible forever; survives the copy
(FUN_140885500 copies +0x60). Grace-local (own object); player dot/objectives/fog/markers are
separate. Field map + recipe in `docs/re/windows_grace_warppin_suppression_re_findings.md`.

**Grace pin DRAW vs TELEPORT — COUPLED at _visible (2026-06-22, re_v122..v124).** The +0x60
state-zero suppression (d8d2125) ALSO breaks fast-travel: each grace pin draws as ONE GFx widget,
and `vt[1] SetTo FUN_14087ae20` sets `widget._visible = pin+0xC` (`FUN_140733340`→`FUN_140d844d0`).
The map cursor only snaps to a _visible widget, so +0x60→+0xC=0 removes the pin from input → no
warp. Draw & click share _visible; there is NO separate per-pin draw-vs-hit flag. Teleport warps by
`pin+0x54` (id) / `pin+0x240` (sub-record) once selectable. The ONLY draw-only lever = the `Icon_0`/
`IconImage` child (fed by per-layer icon sub-structs pin+0x248/+0x288/+0x2c8/+0x308 via `vt[12]
FUN_14088bb60` + `FUN_14074bcc0`): an invalid frame/rect hides only that child, outer widget stays
visible. **Field-poking the pin DOESN'T stick** (icon sub-structs re-filled every SetTo — same layer
trap as +0xC). DRAW-ONLY FIX = hook `vt[1] SetTo FUN_14087ae20`, re-hide Icon_0 (`FUN_140733340(
Icon_0,0)`), keep +0xC/+0x60 untouched; guard by object vtable (shared with point pins). One
container/widget per pin (renderer + cursor both walk VM warp list VM+0x2d8). grace_suppress_native
stays OFF by default until the SetTo fix lands. Full map: `docs/re/windows_grace_warppin_teleport_re_findings.md`.

**SetTo Icon_0 hide — GFx proxy ABI SOLVED (2026-06-23, re_v125).** Tactic A (hook vt[1] SetTo
FUN_14087ae20, hide the "Icon_0" GFx child to draw-suppress graces while keeping teleport) crashed
0xC000001D — ONE wrong arg. The GFx child proxy = ComponentProxy@+0 + **CSScaleformValue@+0x28**;
`release FUN_140d7f850` operates on the CSScaleformValue, so it must be called on **proxy+0x28**, NOT
the base (releasing the base rewrote the ComponentProxy vtable and called garbage). get_child
FUN_14074a2f0(widgetRoot, &proxy, "Icon_0"=utf8 char*) and set_visible FUN_140733340(proxyBASE, byte)
were already correct. Verified vs SetTo disasm: get_child→RSP+0x40, release→RSP+0x68 (=+0x28); final
release(widgetRoot+0x28) same idiom. Fix shipped (release(child+0x28)), kSetToHookEnabled re-enabled, built+deployed.
**2026-06-23 RUNTIME: NO crash + teleport OK, BUT the native grace is STILL VISIBLE (effet d optique) — hiding the "Icon_0" child did NOT remove the grace sprite. So "Icon_0" is NOT the grace icon node, OR the grace draws via a different child/sprite. DRAW SUPPRESSION STILL UNSOLVED; only the +0x60 zero actually hid it (but that breaks teleport). Over-allocate proxy 0x60B zeroed (real ~0x58). Safer §5 fallback
(hook FUN_14074bcc0 with TLS grace filter + invalid descriptor) documented but not needed. Full ABI:
`docs/re/windows_grace_warppin_setto_abi_re_findings.md`.

**Map cursor selection — game-side, gates on pin+0xC NOT on the GFx row (2026-06-23, re_v126..v129).**
Decisive for the draw-only fix. The world-map control (ctor FUN_1409be5e0) binds GFx "WarpList" ⇄ VM
warp list (VM+0x2d8) via the generic controller FUN_1409ca380 = CS::WorldMapItemControl (vtable
0x2b2c7e8, +0x18=VM list). Selection = **vt[6] FUN_1409cab60** = nearest pin to the cursor reticle
(WorldMapCursorControl+0xFC/+0x104) among pins with **`*(char*)(pin+0xc) != 0`** (+ zoom pred vt[10]),
reading pin pos via vt[4]. So selection reads **pin+0xC from the WarpPinData object** (that's why
+0x60→+0xC=0 killed teleport) — but it does **NOT** read the per-pin GFx ROW's _visible. ⇒ DECOUPLE:
hide the GFx row (set widgetRoot _visible=0, NOT pin+0xC) → draw gone, selection still sees pin+0xC=1
+ pos → teleport works. Implemented: warp_setto_detour now does `g_gfx_set_visible(widgetRoot,0)`
(=FUN_140733340(widgetRoot,0), the call SetTo itself makes) — dropped the crash-prone Icon_0/proxy
dance entirely. Built+deployed, TO RUNTIME-TEST. Fallback = row _alpha=0. Doc:
`docs/re/windows_grace_warppin_cursor_re_findings.md`.

**SetTo row-hide ROOT CAUSE (2026-06-23, runtime log [SETTO]).** Calling set_visible(widgetRoot,0)
AFTER orig(SetTo) is a NO-OP: widgetRoot is a per-call STACK GFx proxy (same addr 0xe19… for every
pin) that SetTo RELEASES at its end via FUN_140d7f850(proxy+0x28). So post-orig the proxy's underlying
is dead → FUN_140733340 gets a null underlying → does nothing (same reason the old Icon_0-child hide
failed). FIX (deployed, to test): in warp_setto_detour, force **pin+0xC=0 BEFORE orig** so SetTo's OWN
internal set_visible(widgetRoot,0) runs while the proxy is live → row hidden; then **restore pin+0xC**
after orig so cursor selection vt[6] FUN_1409cab60 (reads pin+0xC + pos) keeps the grace clickable →
teleport survives. Single-threaded engine, SetTo and vt[6] are separate passes so the toggle is safe.
No set_visible call / no proxy handling needed now.

**Hide overlay when a map sub-dialog (Warp/Teleport list) is open (2026-06-23, re_v130..135).**
Problem: our ImGui overlay draws via Present AFTER the game's Scaleform, so when the fast-travel /
Warp Select dialog opens OVER the world map our icons cover it. No ER "clip my external overlay" API
exists → must read game UI STATE and gate OFF. SIGNAL: `WorldMapDialog+0xa58` (signed char) = active
sub-panel index, set by switchSubPanel `FUN_1409df3b0(dialog, idx)` (idx 0 = Warp Select via
createSubDialog FUN_1409df230→factory 0x9e4740→ctor FUN_1409e31c0). The fn's own `if (cVar1 < 0)`
proves **<0 = NO sub-panel (plain map browse), >=0 = a modal sub-dialog is up**. +0xa58 is on the same
WorldMapDialog the probe already resolves (next to +0xa44/+0xa88). Shipped: probe exposes
LiveView.subPanel; render_markers early-returns (overlay OFF) when subPanel>=0. Built+deployed with a
throttled [SUBPANEL] confirm log (TO runtime-confirm: <0 browse, >=0 warp). Doc:
`docs/re/windows_grace_warppin_cursor_re_findings.md` (registry) + this. WorldMapWarpSelectDialog
vtable 0x2b31f68, size 0x1b60.

**⚠ +0xa58 sub-panel gate = DEAD END (2026-06-23, runtime [SUBPANEL] log).** Reading
`probeDialog+0xa58` returned a CONSTANT 64 (never -1/0) during normal map browse → the gate
`subPanel>=0 → overlay OFF` hid the overlay PERMANENTLY ("ne rend rien"). Means switchSubPanel
FUN_1409df3b0's `param_1` is NOT the same object as the probe's WorldMapDialog (cursor−0x2DB0) — the
0xaXX offsets overlap by coincidence but it's a different object. Reverted the gate + the
LiveView.subPanel field. Lesson: an index field at +0x… that indexes array[idx] can't read 64 on the
right object → wrong base. Next: the user wants a GENERAL HUD clipper (exclude our overlay from ER's
HUD element screen rects) — auto-detecting Scaleform HUD rects is impractical/fragile; tractable
version = config-driven normalized exclusion rects that cull our markers + clip the minimap.

**FD4 CPU DDS cache = NOT reachable from CSTextureImage (2026-06-23, re_v136..v139).** Hypothesis
"FD4 keeps decompressed menu DDS in CPU RAM, reachable by name" = effectively FALSE for our purpose.
The CSTextureImage (find-by-name return, vtable 0x2bb8910) is a GPU sub-image descriptor: +0x10/+0x38
= HAL/Render::Texture refs, +0x28..+0x34 dims, +0x70 sub-image flag, +0x74/+0x7c/+0x84/+0x88 rect. The
image factory FUN_140d650b0 just makes a HAL texture view copying the GPU resource handle (hal+0x80 =
ID3D12Resource) over an existing sheet — NO CPU bytes stored. "DDS "(0x20534444)/TPF(0x465054) magics
have 0 code refs (parsed field-wise in the resource/oodle layer, not transiently surfaced). ⇒ brief Q2
impossible; recommend the FALLBACK = self oodle-decompress mod/menu/hi/01_common.tpf.dcx via
oo2core_6_win64.dll OodleLZ_Decompress → parse TPF → DDS → our create_tex_from_dds uploader (own SRV,
crop by the by-name rect). Keeps GPU-harvest as the fast path. Doc:
docs/re/windows_fd4_ram_dds_cache_re_findings.md.

**Map-point iconId→rect is NOT in eldenring.exe (2026-06-23, find_pinicon).** Texture pipeline SOLVED
upstream (Oodle IAT-hook grabs ER's transient decompressed DDS → own texture mgr → sheet-as-atlas; item
iconId→rect via find hook name MENU_ItemIcon_<id>). Remaining gap = MAP-POINT icons: they ride the same
per-pin icon-descriptor path as warp pins — point pin (WorldMapPointPinData vt 0x2ad6688) vt[1] SetTo
FUN_14087ae20 → FUN_14074bcc0(Icon_0, desc) where desc = vt[12] FUN_14087bf20 → pin+0x250/+0x290/+0x2d0.
The sub-rect is resolved from the gfx/sblytbnd layout, NOT an iconId-keyed rect in the exe → that's why
the find hook (repo+0x80 by-name tree, er+0x3d82510) never surfaces map-points. ⇒ Lead 1 (exe-side
iconId→rect table) = DEAD END. Authoritative source = icon-layout .sblytbnd (.layout SubTexture name x y
w h) + iconId→SubTexture-name convention (ASSET data, not code). Recommend lead 3 offline-bake
(tools/extract_subtextures.py → generated iconId→(sheet,rect), per-profile) or lead 2 runtime
(force_load_file the worldmap sblytbnd via FUN_140d771d0 → Oodle hook → parse). CRUX both = iconId→name,
answered by inspecting the .layout names. Doc: docs/re/windows_map_point_icon_layout_re_findings.md.

**Map-point iconId→rect SOLVED via sblytbnd (2026-06-23).** Dumped 01_common.sblytbnd.dcx
(tools/dump_layout_names.py, SoulsFormats+pythonnet). Map-point icons = **SB_MapCursor.layout → sheet
SB_MapCursor.png** (~2048×1024), name-encoded **MENU_MAP_<NN>.png** where NN = WORLD_MAP_POINT_PARAM
.iconId (zero-padded 2-digit: 04,05,06,07,08,09,10,11,13,14,16,17,18,20,21,22,23,24,25,26,27,28,29,30,
50). So iconId→"MENU_MAP_%02d" is DIRECT; entry has x/y/w/h → UV=rect/sheetdims. Special/MP markers
named (MENU_MAP_Church/Enemy_0x/Friend_0x/Host/Player/Range/memo_2x). ERR custom = SB_MapCursor_ERR
.layout → SB_MapCursor_ERR.png (8: MENU_MAP_ERR_Boss/Camp/Bounty/GraceUnderground/Remembrance/BlueTower/
Completed) — their iconId→name from the ERR param per profile. No gfx frame→symbol needed for standard.
Runtime capture saw hasLayout=false because the icon sblytbnd loads at BOOT (before the Oodle hook) →
lead 2 needs force_load; lead 3 offline-bake needs no runtime. Doc:
docs/re/windows_map_point_icon_layout_re_findings.md.

**RAM map-icon layout capture shipped (2026-06-23).** Instead of offline-bake, capture the sblytbnd
layout at runtime: force_load_file("menu:/01_common.sblytbnd") (raw CSFile FUN_1401f5560, fresh load
since it's boot-resident) → ER Oodle-decompresses → our existing Oodle IAT hook catches the BND4. Fix:
the .layout entries are PLAIN-TEXT XML in the decompressed buffer, so detect by ASCII "<SubTexture"
(memmem_simple), NOT the old UTF-16 ".layout" scan (never matched). parse_icon_layout() scans the
buffer for <SubTexture name x y width height>, fills g_map_icon_rects (iconId from MENU_MAP_<NN>) +
g_map_icon_named (ERR_*/Church/etc), logs [MAPLAYOUT] N. Getters: goblin::map_icon_rect(iconId,...),
map_icon_rect_by_name, map_icon_layout_count. F1 button "Capture map-icon layout (sblytbnd)" triggers
the force-load (gated on dumpIconTextures). NEXT = wire MapPointProvider to draw {captured SB_MapCursor
DDS sheet, uv=rect/dims}. goblin_inject.cpp.

**Map-point iconId→rect — RAM SOURCE FOUND = the resident image repo (2026-06-23, live RPM).** The
sblytbnd-hook path is a DEAD END: confirmed via offline decompress + live heap scan — the decompressed
sblytbnd BND4 (.layout XML <SubTexture> in clear, 659360 B) is FREED after boot parse; `<SubTexture`
exists NOWHERE in the live heap; force_load returns a cached file-wrapper resource (path+meta, no BND4)
and is itself transient. BUT the parsed names MENU_MAP_* ARE resident, and **walk_map_repo.py proved
the repo (er+0x3d82510, by-name tree repo+0x80 AND twin repo+0xb0) already holds every MENU_MAP_*
CSTextureImage with rect @img+0x74..0x80 = (x0,y0,x1,y1) + sheet @ [img+0x10]+0x70, vt_ok, map open.**
e.g. MENU_MAP_Church (1660,150,1800,338) == offline x1660 y150 w140 h188. So find-by-name DOES resolve
map icons (the find HOOK just never logs them). RUNTIME-CONFIRMED 2026-06-23: [MAPRECT] logs show real rects (MENU_MAP_Church (1660,150)-(1800,338), ERR_Boss/Camp on SB_MapCursor_ERR, etc) — iconId->rect+sheet read from RAM works. FIX was: MENU_MAP_* live in repo+0x80 (the by-name tree, NOT the +0xb0 twin which holds MENU_MapTile_*); added a MENU_MAP_ branch to harvest_repo_icons (+0x80 walk). Rect read = img+0x74..0x80 (same as cache_icon_from_img). 3 sheets: SB_MapCursor 0x..98d0, SB_MapCursor_ERR, + a 3rd for big NN. SHIPPED: the existing §8c twin-walk
(harvest_twin_map_icons→cache_map_sprite_from_img) now also calls store_map_icon_rect → fills
g_map_icon_rects (iconId from MENU_MAP_<NN>) + g_map_icon_named (+sheet); getters goblin::map_icon_rect
(iconId,x,y,w,h,sheet&) ready for the MapPointProvider. Auto-fills on map open, no decompress/force-load.
Tools: tools/dump_layout_names.py, probe_dcx.py, walk_map_repo.py. sblytbnd csfile/oodle-window hook is
now superseded (can be removed).

**▶ DLC WORLD→MAP-SPACE PROJECTION — NO "EYEBALL" (verdict 2026-06-27, docs corrected).** The
roadmap 🥉 "DLC worldmap projection: eyeball constants never solved" is a NON-ISSUE / already solved.
The live converter dump (world_to_mapspace findings §1) shows area **60 and 61 carry IDENTICAL**
`scale 1.0 / originX 7168 / originZ 16384 / bias 128,128` (→ `−7040/+16512`), and area 61 = **DLC
overworld** (map_renderer.cpp:794); base underground (area 12) **shares the overworld converter** — only
the PAGE byte differs ([0=OW,1=UG,10=DLC]). So **OW, DLC-OW and UG use the SAME world→map-space affine**;
the baked fallback `world_to_mapspace_xy` (−7040/+16512) already projects DLC overworld correctly (my
"wrong-position flash" worry was wrong). `config::liveProjection` adds the **LegacyConv dungeon fold**
(legacy + DLC-legacy 40–43) + page assignment — NOT a different DLC affine. The stale "per-page origins /
scale 0.5 / DLC own origin" claims live in the OLD `marker_to_mapspace_re_findings.md` (RE-era hypothesis,
superseded). Docs fixed on master (commit d2556cd): SUPERSEDED banner on the old doc + RESOLVED notes on
`windows_world_to_mapspace_projection_re_findings.md` TL;DR-Q3 + §7.1. Nothing to bake/capture.
