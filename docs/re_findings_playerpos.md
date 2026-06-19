# RE findings — player world/map position (Target A)

Static Ghidra RE (project `D:\ghidra_proj2\ER.gpr`, Ghidra 12.1.2; headless scripts
`D:\ghidra_scripts\re_v13..v18.java`) for `docs/windows_re_briefing_playerpos_questbrowser.md`
**Target A** (a stable player position for proximity clustering v2). App **2.6.2.0**.

> **Confidence: STATIC-derived, NOT yet live-verified.** The chain + offsets below come
> from the decompiler; the exact manager singleton (3 candidates) and the "continuous
> across chunks" property must be confirmed live — recipe in
> `D:\DOWNLOAD\MapForGoblins_verify_playerpos_recipe.md`.

## Headline

The old CT chain is **dead on 2.6.2.0**: `LocalPlayerOffset 0x10EF8` now occurs **once** in
the whole binary (drifted). But the engine already computes a **player position in MAP /
MARKER space** (same units as `WorldMapPointParam.posX/posZ` and the map cursor) — so for
clustering we want *that*, and **no chunk→world bridge is needed**.

> ⚠️ **CORRECTION (2026-06-18, scripts `re_v19`/`re_v20`, decompiled).** The "already in
> marker space, no bridge needed" claim above is **WRONG**. The whole builder→reader→projector
> path is a **pure Vec copy with no arithmetic** — no `256`, no `1/256`, no floor/division, no
> grid index anywhere. So `manager+0x70/+0x78` is just the **raw block-local physics Vec**,
> and a chunk→world bridge **IS** needed. See "v19/v20: the path is a pure copy" below.

## The chain

- **WorldChrMan static:** RVA **`0x3d65f88`** (VA `0x143d65f88`). AOB
  `48 8B FA 0F 11 41 70 48 8B 05` `{{0xA, 0xE}}` (mov rax,[rip+disp] at +7; slot =
  finder + 0xE + disp32@+0xA). Resolved live, correct for 2.6.2.0 — **no drift**.
- **New player field:** `[WorldChrMan + 0x1e508]` (replaces the dead `+0x10EF8`).
- **Player MAP-pos builder `FUN_1406d3a20`** (RVA `0x6d3a20`): writes the player map position
  into a manager struct at **`+0x70` (X) / `+0x74` (Y) / `+0x78` (Z)** (+ a smoothed/camera
  copy at `+0x80/+0x84/+0x88`). Two source paths:
  - main: `FUN_1403f0bf0` → `FUN_1403c6ff0` reads `[[[WorldChrMan+0x1e508]+0x58]+0x10]+0x190 …]`
    and fills a Vec2;
  - fallback `FUN_1406614d0`: reads `obj+0x70`/`obj+0x78` directly.
  Either way the **position layout is `+0x70`=X, `+0x78`=Z** (Vec2, marker space).
- **Manager update `FUN_1406d31f0`** (RVA `0x6d31f0`, ← `FUN_140623410` RVA `0x623410`): per
  call it (1) builds the player pos at `manager+0x70/+0x78` via `FUN_1406d3a20`, then (2)
  walks a red-black tree of map points at `manager+0x20` and tests each against `manager+0x70`
  — i.e. this is the **world-map point / discovery manager**, and `manager+0x70/+0x78` is the
  live player position it uses.

## Live-readable handle (to verify)

`manager` is a world-map FD4Singleton; `FUN_140623410` updates several
(`DAT_143d692f8`, `DAT_143d69380`, `DAT_143d69ba8`, … RVAs `0x3d692f8` / `0x3d69380` /
`0x3d69ba8`). The one passed to `FUN_1406d31f0` is the map-point manager — **pin it live**:
read `[eldenring.exe + 0x3d69380] + 0x70/0x78` (try the 3 candidates), move the player, see
which changes. (`CSWorldMapPointMan` @ RVA `0x3d6e9d8` from the older RE is a 4th candidate.)

## Why this answers Target A

The briefing wanted a *stable, continuous* player position because the raw chains went
NaN / chunk-local. `manager+0x70/+0x78` is **already in marker space** (the projection the
map itself uses), so it is continuous across chunks by construction and directly comparable
to `WorldMapPointParam.posX/posZ` — proximity clustering can use it with **no calibration**.

## AOBs (entry; extend for uniqueness)

| RVA | function | AOB |
|---|---|---|
| 0x6d3a20 | player map-pos builder | (site AOB) `48 8B FA 0F 11 41 70 48 8B 05` is at RVA 0x6d3a4e inside it |
| 0x3f0bf0 | projector wrapper | `40 53 48 83 ec 20 48 8b 49 58 48 8b da e8` |
| 0x3c6ff0 | main-path pos reader | `40 53 48 83 ec 20 48 8b 41 10 48 8b 88 90 01 00 00 48 8b 49 68` |
| 0x6d31f0 | manager per-frame update | `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 e9 48 81 ec e8 00 00 00` |

Static globals: WorldChrMan `eldenring.exe+0x3d65f88`; player field `[WCM]+0x1e508`;
manager candidates `+0x3d692f8 / 0x3d69380 / 0x3d69ba8`; pos at `manager+0x70`(X)/`+0x78`(Z).

## Remaining (live, Cheat Engine — to run on Windows)
1. Pin the manager singleton (read `[+0x3d69380]+0x70/0x78` etc.; the one that tracks the
   player as you walk).
2. Confirm `+0x70/+0x78` == `WorldMapPointParam` space (compare to a nearby marker's posX/posZ).
3. **Walk across a map-block boundary (m60_XX_YY → adjacent)** → confirm `+0x70/+0x78` stays
   continuous (no reset/jump) — the property the old chains lacked.
4. Confirm it updates with the **map closed** too (clustering runs in-world, not just in-menu).

## v19/v20: the path is a pure copy (CORRECTS the headline)

Decompiled the full builder→projection path (`D:\ghidra_scripts\re_v19.java`, `re_v20.java`;
logs `re_v19_out.log` / `re_v20_out.log`):

```c
// projector — just a Vec getter, NO math:
FUN_14045e390(src, out){ out[0]=*(src+0x70); out[1]=*(src+0x78); return out; }
// reader:  src = *(*(*(a+0x10)+0x190)+0x68),  a = *(player+0x58)
FUN_1403c6ff0(a){ FUN_14045e390( *(*(*(a+0x10)+0x190)+0x68) ); }
// wrapper: player = [WorldChrMan+0x1e508]
FUN_1403f0bf0(player){ FUN_1403c6ff0( *(player+0x58) ); }
// builder writes manager+0x70/+0x78 directly from that copy (FUN_1406d3a20)
```

So **`manager+0x70/+0x78` = a raw block-local physics Vec**, sourced from
`[[[ [WorldChrMan+0x1e508]+0x58] +0x10] +0x190] +0x68] + 0x70/0x78`. Constant scan of the
builder, manager-update (`FUN_1406d31f0`) and reader found **no `256` / `0x100`, no
`1/256=0.0039`, no floor/division, and no gridX/Z field** — the only floats are timers
(2.28, 52.4, 3.7); the only accumulated ints (`+0xf0..+0x100`) are map-point discovery
counters. The manager-update does reference **`CS::CSWorldBlockGeom`** (the per-block world
geometry / block origins) — that is the likely home of the block→world offset if a bridge is
built.

## The marker coordinate system is already solved in this repo

`tools/extract_markers.py` (grace-verified, error < 2 units) is the authoritative transform:

```python
OFFSET_X = 7042; OFFSET_Z = 16511        # map-UI  <-> world
worldX = mapX + OFFSET_X;  worldZ = -mapZ + OFFSET_Z
gridX  = int(worldX // 256); posX = worldX - gridX*256     # world <-> tile, CELLSIZE = 256
```

So: marker continuous world = `gridXNo*256 + posX` (NB the game's `posX/posZ` are **signed and
not normalized** to [0,256) — range observed across the CSV is roughly −880..+1620 — but the
sum `grid*256+pos` still reconstructs world). The map cursor (`+0xFC`, live 3183→4057) is in
map-UI space; world = mapUI + offset. `manager+0x70/+0x78` (live −2.77 / −24.08) is in
**neither** (world ≈ 10–12k, map-UI ≈ 3–4k) → it is the block-local physics space.

## DECISIVE live test (forks the whole plan — one observation)

Manager pinned live = candidate **`eldenring.exe+0x3d69ba8`** (P=`0x1BAA164E680`,
`P+0x70`/`P+0x78` floats). **Walk across a map-block boundary watching `P+0x70`/`P+0x78`:**
- **Stays continuous** (no reset/jump) → it is global/world space after all → clustering can
  use it directly (compare to `gridX*256+posX`); **Target A is DONE**.
- **Resets / sawtooths at the boundary** → it is block-local → a block-origin bridge is needed.
  Next: read the player's current MapId / `CSWorldBlockGeom` block origin and compute
  `world = block_origin + local` (then `grid = world//256`).

(Earlier the "sawtooth ±32" was seen on *some* CE coord; confirm it specifically on the
pinned `P+0x70/+0x78` so the fork is decided on the right field.)

## LIVE confirmed (2026-06-18, candidate `+0x3d69ba8`) — block-parent-relative w/ hysteresis

Walked in-world watching the pinned `P+0x70`/`P+0x78`:
- **Axes:** `+0x70` = X (East = +), `+0x78` = Z (North = +); South/West negative.
- **Sawtooth amplitude is ±32, NOT ±128.** Crossing chunks north repeatedly: `0→32→0→32`
  (resets at every boundary); south: `0→-32→0→-32`. So the streaming cell of THIS coordinate
  ≠ the 256-unit marker tile — either the cell is 32 units here or there is a **~×8 scale**
  (256/32) between this local and marker `posX`. **To pin live.**
- **Directional hysteresis (the "logique chelou"):** go north across a boundary `…→32→0`, then
  reverse — instead of `0→32` it reads `0→-32`. Diagnostic, not a bug: the value is the player
  position **local to the current parent world-block**, and the streaming **re-parents** the
  player at each boundary (transform recomputed against the new block's origin; sign flips
  because the entry edge flips). So `local` is **not a pure function of position** — it depends
  on the parent block and entry direction.
- **Implication:** `world = grid·256 + local` is invalid as-is. BUT reading the **current cell
  index live, together with `local`, cancels the hysteresis** (both come from the same parent
  assignment): `world = idx·cellsize + local·scale`.
- **Decisive live step:** pin the int that does **+1 at each boundary** (CE: "increased value
  by 1" scan right after a crossing) = the player's `gridZNo`/`gridXNo` (or a sub-cell index —
  compare to a known grace's CSV `gridXNo/gridZNo` to learn whether idx is the 256-tile or a
  sub-cell, and to get the scale).
- Pipeline convention cross-check: `tools/generate_maps.py` writes marker `gridXNo = tile XX`
  + `posX = raw MSB-local position` — same (tile, block-local) shape as the player's
  (idx, local); we just need the scale + idx to bridge.

### v21 (static): no scale constant; units are 1:1; block system = CSWorldBlockGeom

`D:\ghidra_scripts\re_v21.java` (log `re_v21_out.log`):
- The alt branch `FUN_1403bb1c0` is `return *(param+8)` and the fallback `FUN_1406614d0` is the
  same `+0x70/+0x78` copy → **no cleaner absolute-position field exists on these paths**; every
  branch copies the same block-local Vec.
- **Scale hunt found NO scale constant** (no 256 / 1-over-256 / 8 / 32 float in builder/reader/
  alt/fallback). The only `±32` hits are `SUB 0x20`/`ADD 0x20` = the reader's stack prologue,
  not coordinate math. ⟹ the **±32 is intrinsic to the source physics coordinate**: the
  physics world-block is ~64 units (half = ±32), and local↔`posX` is likely **1:1 in units** —
  what differs is the **block size** (physics ~64 vs marker tile 256 → ~4 physics blocks per
  256 tile, so the +1-per-boundary index counts 64-unit blocks, and `gridXNo = idx // 4` to
  confirm).
- `CS::CSWorldBlockGeom` located: `CSWorldBlockGeomUpdaterMT::AddBlock()` string xref'd from
  **`FUN_1406d2d80`** (RVA `0x6d2d80`) — block add/origin lives here (decompile target if the
  empirical calibration below isn't enough).

### Final bridge plan (empirical, 1 grace settles everything)

Clustering v2 doesn't need a global continuous coord — only tile-aware proximity. We have
markers as (gridXNo, posX) from the CSV; we need the player as (idx, local). Steps:
1. CE: pin the int(s) that **+1 per boundary** (= player tile/block index for X and Z).
2. Stand on **one known grace**, record: player `+0x70`/`+0x78` (local) and player idx X/Z;
   compare to that grace's CSV `posX/posZ` and `gridXNo/gridZNo`. This single reading gives
   **both** the unit scale (player local vs grace posX) **and** the index↔gridXNo relation
   (player idx vs grace gridXNo — is idx the 256-tile or a 64-unit sub-block ⟹ //4).
3. Bridge: `markerX = gridXNo_player·256 + posX_player`, with `gridXNo_player = f(idx)` and
   `posX_player = scale·local` from step 2. Then proximity vs every marker is direct.

### SHORTCUT LEAD (2026-06-19): reuse a teleport cheat-table's coordinate pointer

Live-testing surfaced a much faster route. A working **teleport / warp** cheat table must
read+write the player position, and ER teleport coords are typically the **global world
position** (continuous, thousands-range), not the ±32 block-local Vec. If so, that pointer
**is** Target A — skip the whole gridZ/idx hunt.

Test (decisive): add the CT's coordinate entry (the X/Y/Z the warp uses) to the address list,
stand on **The First Step** (tile gridX=42, gridZ=36) and read it.
- World-space check: First Step world ≈ **X≈10740, Z≈9263** (`grid·256 + local`; from the
  "First Step – Tree Sentinel" mark local ≈ −12 / +47). If the CT coord reads ~these
  thousands-range values and stays **continuous across block boundaries** → it is the global
  player position → use it directly for clustering; AOB/pointer-scan it for the DLL.
- If it still reads ±32 + a separate **MapId**, then the warp uses (MapId, local) — which hands
  us exactly the block-origin table we were missing (origin indexed by MapId).

Either outcome advances Target A; capture the CT's **pointer path (offsets)** for the coord so
it can be reproduced from the DLL without the cheat table.

### Map live-rebuild cross-link

Also observed live: **"Show All Grace" rebuilds the open world map in real time** → recorded in
`docs/world_map_live_refresh_re.md` as the on-demand icon-rebuild lead for the section-toggle
live-refresh thread (`FUN_140a832a0` / `_DiscoverMapPoint`).

## BREAKTHROUGH (2026-06-19): no global coord exists — bridge = block-local + MapId tile

Confirmed empirically + statically that ER stores **NO single global world float** (seamless-world
float-precision avoidance). Position = **block-local + block identity**. So clustering must do
`player (tile, local)` vs each icon `(gridXNo·256 + posX)` — same space, no global needed.

**Live confirmation (CT `eldenring.exe+0x3d69ba8`, 3 graces):**
- Manager pos layout **CORRECTED**: `+0x70`=**X**, `+0x74`=**Z**, `+0x78`=**height(Y)**, `+0x7C`=1.0
  (w). (Prior note said `+0x78`=Z — wrong; `+0x78` is height.) `+0x80..0x88` = smoothed/cam copy.
- These == the console-mod `coords` (block-local, recentred ±32). `+0xE0..0x140` = garbage at all 3
  graces, unchanged with map open → **no map-UI/global field here; the old `+0xFC` lead is DEAD.**
- The console mod (`er_console_mod.dll`, Nexus 9365) `coords` reads this same block-local; its `tp`
  writes a coarser local (within-block ~0-256) frame — far/cross-region `tp` = unloaded geom =
  soft-lock. Neither is global. See [[er-console-mod]].

**MapId byte layout DECODED** (Ghidra `re_v23`, producer `FUN_140660e80` of `"m%02d_%02d_%02d_%02d"`):
a 4-byte packed id, little-endian `*(uint*)p`:
```
byte[3] (p+3) = area    (0x3C = 60)
byte[2] (p+2) = gridX   (0x2A = 42)   <- TILE X
byte[1] (p+1) = gridZ   (0x24 = 36)   <- TILE Z
byte[0] (p+0) = lod / sub-index
```
So **First Step MapId = `0x3C2A2400`** (area60, gridX42, gridZ36, lod0). (Proves the CT's live
`C14E2803` was garbage: area `0xC1`=193.) Read `byte[2]/byte[1]` of the player's current-block
MapId → `gridX/gridZ`; combine with `+0x70/+0x74` local → world.

**Subsystem = OpenField `WorldGridAreaInfo.cpp`** (classes `WorldGridAreaInfo@CS`,
`CSWorldGridAreaAi`; accessors `FUN_1406333b0 / _140633410 / _140633440(RefWorldBlockInfo)`).
**Open task (`re_v24`):** find where the player's current-block MapId (the 4 bytes above) lives —
either the OpenField grid singleton + offset, or a WorldChrMan field — to get a stable pointer for
the DLL. CT shortcut also possible: scan exact `0x3C2A2400` at First Step, narrow by WALKING (not
warp — warp reallocates), then pointer-scan.

### Live narrowing result (2026-06-19) — the MapId lives in a block-registry RB-tree

CE exact-scan `0x3C2A2400` @ First Step = **2859 hits** (every loaded m60_42_36 object/ref carries
the id → decode confirmed). Narrowed by walking N/S + Next-Scan the expected `…25xx/…24xx`:
2859→500→150 (streaming freed transients)→65→13. These are block-registry nodes (alt-tile
narrowing partly followed reused heap).

**Game access (CE "what writes", crossing a boundary):** `eldenring.exe+0x877640`
`cmp [rax+0x1C],edx ; jae .. ; mov rax,[rax+0x10]` = a **red-black-tree search of loaded map
blocks keyed by MapId**: node `+0x1C` = MapId key, `+0x10` = child link. (The watched field is a
tree NODE's MapId, NOT a direct "player current tile" field.) Search key `edx` for that hit =
`0x63282500` (gridZ 0x25=37 ✓ but area 0x63=99 / gridX 0x28=40 → a *different* block lookup, e.g.
area-99 connect/our injected rows — not the player's). NB our OWN `MapForGoblins.dll+0x5D45E` also
brute-scans a ~108MB heap region here.
**Open (`re_v25`):** decompile FUN@0x877640 + callers → find the tree-root / block-registry
singleton (static base) and the caller that looks up the PLAYER's block (reads player tile from a
stable field) → stable read of player gridX/gridZ for the DLL. Alternative still open: OpenField
grid (`WorldGridAreaInfo`) stored current-cell; or re-pin the clean `gridZNo` int (the one that did
+1/​tile when WALKING early on) and Ctrl+F5 it.

### THE BLOCK→WORLD BRIDGE FOUND (2026-06-19, `re_v25`)

`FUN_1408775e0` (the RB-tree search, entry `0x8775e0`) is the **block re-parent transform**: given
`(MapId, localVec)` it finds the block node and outputs `local + node_origin`:
```
node+0x1C = MapId (key)      node+0x20 = parent MapId
node+0x24 = originX   node+0x28 = originY   node+0x2C = originZ   (block origin in parent frame)
out.x = local.x + node[+0x24] ;  out.y = local.y + node[+0x28] ;  out.z = local.z + node[+0x2C]
```
Iterating up the parent chain (node+0x20) accumulates origins → world.

`FUN_140876140` (entry `0x876140`) is the **(MapId,local) → map-UI** transform that places the
player marker. With `gridX=(MapId>>16)&0xff`, `gridZ=(MapId>>8)&0xff`, area=`(MapId>>24)&0xff`:
```
out.x = (local.x + (gridX - mgr[+0xa])*CELLSIZE - mgr[+0x0c]) * mgr[+0x20] + mgr[+0x18]
out.z = (local.z + (gridZ - mgr[+0x9])*CELLSIZE - mgr[+0x14]) * mgr[+0x20] +/-(signZ) + mgr[+0x1c]
```
mgr (`param_1`) = map-projection manager: `+0x9/+0xa`=base tile Z/X, `+0xb`=area, `+0x0c/+0x14`=
offsets, `+0x18/+0x1c`=final offsets, `+0x20`=scale, `+0x28`=the block tree. `CELLSIZE`=`DAT_1429ce8b4`
(expect 256), signZ flip=`DAT_14329f470` (expect 0x80000000). This is exactly `world = gridXZ·256 +
local` → map-UI (matches `extract_markers.py` OFFSET/scale). **Only missing runtime input: the
player's current MapId** — `re_v26` finds the caller that feeds it (player MapId source + mgr static
base). Then the DLL computes player world from `MapId` (→gridX/gridZ) + `[+0x3d69ba8]+0x70/0x74`
local, no global field needed.

### Transform constants CONFIRMED + dead ends + candidate (2026-06-19, `re_v26`–`re_v30`)

- **Constants confirmed:** `CELLSIZE (DAT_1429ce8b4) = 256.0` ✓, `signZ (DAT_14329f470) = 0x80000000`
  ✓. Formula `world = gridXZ·256 + local` (Z flipped) is locked.
- **Marker-cache chain found but it's NOT the player:** `FUN_1405f6140` →
  `mgr = [[[eldenring.exe+0x3d6b7b0]+0x80]+0x250]`, and `FUN_140886b10` caches a map-UI coord at
  `mgr+0xa0`. LIVE TEST (`player_mapui*.CT`): `mgr+0xa0/+0xa4 = 0` always; `mgr+0xAC = 3700.07`,
  `mgr+0xB0 = 7349.80` **identical at First Step / Academy Gate / Castle Morne (map open or closed)**
  → these are **fixed map constants (extent/centre), NOT the player**. (3700 ≈ First Step's predicted
  map-UI X was a coincidence that misled.) DEAD END for this chain.
- **Player local source (builder `FUN_1406d3a20`):** writes `mgr+0x70..0x7c` from
  `FUN_1403f0bf0([WorldChrMan+0x1e508])` (WorldChrMan = `DAT_143d65f88` = `eldenring.exe+0x3d65f88`).
  It does NOT store a MapId in the manager. Player-exists path = local only.
- **CANDIDATE player/area MapId (UNTESTED):** the builder's fallback (player==null) uses
  `FUN_140243a60(DAT_143d691d8, key)` where `FUN_140243a60 = return *(p+0x2c)`. So
  **`[[eldenring.exe+0x3d691d8]+0x2c]` (4 bytes) = a MapId** read from a heavily-used world/map-state
  singleton. TEST: read it as hex at graces — is it the player's tile MapId (`0x3C2A24xx` @ First
  Step, changing per tile)? `player_mapid.CT` probes it (+0x2c and neighbours).

**STATUS:** formula + MapId decode + player local (`[eldenring.exe+0x3d69ba8]+0x70`=X,`+0x74`=Z)
are SOLID. Missing = a confirmed stable pointer to the player's current **gridX/gridZ** (= MapId
bytes). Best next attempts: (1) test the `[[+0x3d691d8]+0x2c]` candidate; (2) re-pin the clean
`gridZNo` int (the one that did +1/​tile when WALKING at session start) then Ctrl+F5 it (lands in the
OpenField grid, cleaner than the block-registry RB-tree); (3) runtime: breakpoint the map-open
marker calc (`FUN_140876140`) and read the MapId param it receives.
