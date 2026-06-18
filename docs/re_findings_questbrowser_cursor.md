# RE findings — Quest-Browser "show on map" / world-map cursor focus (Target B)

Static Ghidra RE (project `D:\ghidra_proj2\ER.gpr`, Ghidra 12.1.2, headless scripts
`D:\ghidra_scripts\re_v9..v12.java`) answering `docs/windows_re_briefing_playerpos_questbrowser.md`
**Target B**. **All static** (decompiler) — confidence flagged; dynamic verification
recipe in §5 (and the standalone `D:\DOWNLOAD\MapForGoblins_verify_cursor_recipe.md`).

Imagebase `0x140000000`. RVAs are for the analysed build — **resolve by AOB on other
patches** (VMProtect drifts RVAs; the probe re-scans the vtable so it is patch-robust).

## 1. The cursor object — CONFIRMED

Class **`CS::WorldMapCursorControl`** (RTTI-named in the project). vtable RVA
`0x2b29a90` (VA `0x142b29a90`), 3 virtual methods:

| vt | RVA | role |
|----|-----|------|
| vt[0] | `0x7342b0` | `FUN_1407342b0` — registration (registers into a manager) |
| vt[1] | `0x9bc730` | `FUN_1409bc730` — scalar-deleting destructor |
| vt[2] | `0x9bd4b0` | **`FUN_1409bd4b0` — per-frame tick/update** (reads input, moves + clamps the cursor) |

Created by ctor `FUN_1409bc5b0` (RVA `0x9bc5b0`), stored at **menu+0x2DB0**
(`CURSOR_OFF_IN_MENU`, also confirmed by the in-DLL probe).

### Cursor instance fields (offsets) — CONFIRMED writable
Written by the ctor **and** re-written every tick by vt[2]:
```
+0xFC   float  X      (map/marker space — same as WorldMapPointParam.posX)
+0x104  float  Z      (== WorldMapPointParam.posZ)
+0x10C  float  Y
+0xF0   ptr ->  bounds rect:  [+0x340]=minX [+0x344]=minZ [+0x348]=maxX [+0x34c]=maxZ
+0xF8   u8     "has target / snapping" flag
+0x130  float  lerp progress (ctor inits 0.1f);  +0x12c = its duration ref
+0x118  ptr    snap target source (obj; vtable method +0x20 returns target coord)
+0x120  vec    inline snap target coord
+0x134  vec    snap start coord
```
The ctor inits +0xFC/+0x104/+0x10C to `DAT_144802260` (a default). The tick clamps the
written coord to the bounds rect via `FUN_1409cd790`.

## 2. Move / focus mechanism

- **Tick `FUN_1409bd4b0` (vt[2])**: reads `+0xFC/+0x104/+0x10C`, integrates input·speed·dt,
  clamps to `[*(+0xF0)]+0x340..0x34c`, **writes the result back to `+0xFC/+0x104/+0x10C`**.
- **View pan = `FUN_1409bdc50` (RVA 0x9bdc50)**: computes an edge-scroll factor from
  (cursor coord vs the on-screen rect). The camera pans when the cursor nears an edge.
- **Snap-to-target animation = `FUN_1409bc8c0` (RVA 0x9bc8c0)**: a lerp
  `start + ease(elapsed/duration)·(target − start)` using the snap-state fields
  (`+0x118` source / `+0x120` inline target / `+0x134` start / `+0x130` progress).

**Net (mechanism (b) from the briefing — confirmed reachable):** `+0xFC/+0x104` are plain
writable instance floats. Writing them moves the reticle (the next tick re-clamps to
bounds); the view follows via the edge-scroll pan. A centred "jump" is the engine's own
snap-to-target path (the lerp above) — see §4 for the gap.

## 3. Singleton / map-open guard — PARTIAL

A world-map subsystem **FD4Singleton instance pointer lives at RVA `0x3d5dea8`**
(VA `0x143d5dea8`). The cursor tick guards on its **`[+0x883]`** byte before processing
input → **candidate "world map open / ready" flag (deliverable #2)**. It is torn down by
`FUN_140242900` (its destructor) inside the global shutdown `FUN_140dea880`.

⚠️ **Not yet proven** to be `CSWorldMapMenu`, and **not** shown to own the cursor at
`+0x2DB0` (the "menu" that `FUN_1409be5e0` stores the cursor into is a *separate* object).
So the **singleton → cursor chain by AOB (deliverable #1) is unresolved**; until then the
reliable handle is the **vtable scan** (`0x142b29a90`) the probe already uses.

## 4. Open items (for re_v13 or live test)
- **Explicit external setter** "jump cursor to (x,z)" — the function that writes
  `+0x118/+0x120` from outside (a "show on map" / discover entry). Not isolated (the
  vtable methods are not it).
- **CSWorldMapMenu RTTI vtable + its singleton slot**, to give the chain in §3 by AOB and
  confirm whether `0x143d5dea8` is that menu.

## 5. AOBs (entry prologues — extend for uniqueness; one resolved RVA each)

| RVA | function | AOB (entry) |
|---|---|---|
| 0x9bd4b0 | tick vt[2] | `48 8b c4 55 53 56 57 41 54 41 56 41 57 48 8d 68 a1 48 81 ec c0 00 00 00` |
| 0x9bdc50 | edge-scroll pan | `40 53 55 56 57 41 56 48 81 ec 80 00 00 00 48 8b 05 fb 45 e4 03 33 f6 49 8b e8` |
| 0x9bc8c0 | snap lerp | `48 89 5c 24 18 57 48 83 ec 50 80 39 00 48 8b fa 48 8b d9 75 15 48 8b 49 0c` |
| 0x9bc5b0 | cursor ctor | `48 89 4c 24 08 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 48` |
| 0x9be5e0 | menu setup (owns cursor) | `66 44 89 4c 24 20 55 53 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 68 ea` |
| 0x9cef10 | menu open path | `40 55 56 57 41 56 41 57 48 8d ac 24 40 ff ff ff 48 81 ec c0 01 00 00 48` |

Static globals: cursor **vtable** `eldenring.exe+0x2b29a90`; world-map **singleton ptr**
`eldenring.exe+0x3d5dea8` (guard at `[ptr]+0x883`).

## 6. Verification recipe (dynamic — to run on Windows)
Full step-by-step + required software in **`D:\DOWNLOAD\MapForGoblins_verify_cursor_recipe.md`**.
In short:
1. `MapForGoblins.ini` → `debug_worldmap_probe = true`; open the world map in-game →
   `logs/MapForGoblins_wmprobe.log` prints the live cursor address + `+0xFC/+0x104/+0x10C`.
2. Cheat Engine → attach to `eldenring.exe`, add `cursor+0xFC` / `+0x104` as Float.
3. Write a known grace's `posX/posZ` → reticle should jump there + the view pan
   (validates mechanism (b)). Confirms cursor space == `WorldMapPointParam.posX/posZ`.
4. Read `[eldenring.exe+0x3d5dea8]` then `[+0x883]` with the map open vs closed → validates
   the map-open guard (§3).
