# Windows / Ghidra RE briefing — Player Position + Quest-Browser map-reveal

**For:** an agent (or human) on **Windows with Ghidra + the live game** who can attach a
debugger / Cheat Engine. The Linux side (this repo's author) cannot run Ghidra or the
game natively, so this doc hands you two self-contained RE targets with everything the
codebase already knows. Return the deliverables in section 6; the Linux side wires them
into the DLL.

Two targets:
- **A. Player world position** — a *stable, live* global player XYZ read. The chains we
  already have go NaN / are chunk-local. Needed for proximity clustering v2.
- **B. Quest-Browser "show on map" (Phase B)** — programmatically focus the world map on a
  marker's coordinate (and read/confirm the map cursor), so a quest step can reveal its
  location on the map.

---

## 0. Target build & environment

- **Game exe:** `eldenring.exe`, resolved at runtime via `GetModuleHandleA("eldenring.exe")`.
- **Imagebase (Ghidra):** `0x140000000`.
- **VMProtect caveat:** there are **two `.text` sections**. Analyse the **MSVC `.text`** at
  VA `0x140001000` (RVA `0x1000 .. 0x29A3000`), **NOT** the VMP `.text` at `0x144c0e000`.
- **Captured-against build:** ERR mod **2.2.9.6**, the RVAs in-repo were captured against
  **ERR 2.2.1.2 / app ~2.6.x**. A May-2026 patch moved `ShowTutorialPopup` 0x80DA50→0x80D960,
  so **resolve everything by AOB, never by raw RVA** (RVAs drift each patch).
- **Steam path:** `…/steamapps/common/ELDEN RING/Game/eldenring.exe` (~87 MB).
- **Coordinate fact (markers):** `WORLD_MAP_POINT_PARAM_ST.posX/posZ` are **tile-LOCAL**
  (±~500 within a ~1000-unit cell). `gridXNo/gridZNo` = tile index; world tile `XX = gridXNo/2`,
  `YY = gridZNo/2` (each `m60_XX_YY` covers a 2×2 grid-cell block).

### Conventions used by this DLL (match these so the result drops in cleanly)
- **Scan engine:** `Pattern16::scan` over the module image span (base = `AllocationBase`,
  size = `OptionalHeader.SizeOfImage`). `modutils::scan` returns the **address of the AOB's
  first byte**.
- **RIP-relative resolver:** `ScanArgs{ .aob, .relative_offsets = {{first, second}} }` reads
  `*(int32*)(&match[first]) + second` and advances — i.e. for `mov rax,[rip+disp32]`,
  `{{3,7}}` = disp32 at byte +3, instruction length 7. Always express a static-slot find as an
  AOB + a `{{first,second}}` pair.
- **Deliver static addresses as `(AOB, relative_offsets)`**, function entries as a bare AOB.

---

## A. Player world position (live, global, stable)

### What the repo already has (all flagged "VERIFY / goes NaN")
Dormant reader `goblin::get_player_world_pos(float&,float&,float&)` —
`src/goblin_inject.hpp:18`, impl `src/goblin_inject.cpp:601`. WorldChrMan static resolver
`resolve_world_chr_man()` at `src/goblin_inject.cpp:535`:

```
WorldChrMan static:  AOB  48 8B FA 0F 11 41 70 48 8B 05
  trailing 48 8B 05 = mov rax,[rip+disp32] at +7 (ends +0xE)
  static slot = finder + 0xE + *(int32*)(finder+0xA)
```

Two candidate coordinate chains are probed simultaneously (`PlayerProbe`, `:553`), both
from **Hexinton all-in-one CT v6.0**, both unreliable:

- **Candidate A (CT "global"):** `[[[WorldChrMan]+0x10EF8]+0]+0x6B0/0x6B4/0x6B8` = X/Y/Z.
  `0x10EF8` = LocalPlayerOffset. → In-game this reads a **CE-computed buffer that goes NaN
  live** (it's not a real engine field).
- **Candidate B (physics):** `[[[[player]+0x58]+0x10]+0x190]+0x68 (+0/+4/+8)` = X/Y/Z.
- A third, separately-documented chain (`docs/windows_csworldmapmenu_re_prompt.md:74`):
  `[[[[[WorldChrMan]+0x10EF8]+0]+0x190]+0x68]+0x70/74/78` = X/Z/Y (order X,Z,Y) — **chunk-
  relative** (resets per map chunk), not global.

**Net:** we have a reliable `WorldChrMan` base and `LocalPlayerOffset 0x10EF8`, but **no
clean, stable, global live XYZ**. The CT "global" is a tool-side buffer; the engine field is
chunk-local.

### The ask (Target A)
Starting from `WorldChrMan` + `[+0x10EF8]` (LocalPlayer / `PlayerIns`/`ChrIns`):
1. Find the **PhysicsModule / FieldArea world transform** that yields a **stable global**
   position (i.e. the value the map/compass uses, continuous across chunk boundaries — not the
   `+0x190→+0x68→+0x70/74/78` chunk-local one). In ER this is usually the `CSFD4LocationCategory`
   / `FieldArea` map-id + a local offset combined into a world coord; identify the offset chain
   and whether a `mapId` (block/grid) must be combined.
2. Confirm **axis order and units** (X,Y,Z vs X,Z,Y; metres vs cell-units) by walking around a
   known landmark and comparing to a `WORLD_MAP_POINT_PARAM_ST` row's `gridXNo/gridZNo` + posX/posZ.
3. Note whether the value is valid only when loaded into a field map (vs menus/load screens) and
   the cleanest "is the player in-world" guard.

### Deliverable (Target A)
- The **offset chain** from `WorldChrMan` to a stable global X/Y/Z, each step annotated.
- If a `mapId`/grid combine is needed: the offsets of the grid indices + the formula to fold
  local pos + grid into a continuous world coord (mirror `XX=gridXNo/2` marker convention).
- An **AOB** for any new static/function you rely on, with `{{first,second}}` if it's a slot.
- 2–3 verification samples: `(landmark name, raw chain values, expected world tile)`.

---

## B. Quest-Browser "show on map" — focus the map on a coordinate (Phase B)

Goal: from the in-overlay Quest Browser, a per-step **"Show on map"** button that opens/uses
the world map and **moves the cursor / centres the view** on that step's location (a marker
region or an explicit map coord), then optionally drops/highlights a pin.

### What the repo already has — the cursor probe
`src/goblin_worldmap_probe.cpp` (+ `.hpp`), gated by `config::debugWorldmapProbe`
(INI `debug_worldmap_probe`, logs to `logs/MapForGoblins_wmprobe.log`). It is a **read-only
memory scanner** (no hook): it scans committed `MEM_PRIVATE` regions for a qword equal to
`base + CURSOR_VTABLE_RVA` (the cursor object's first qword == its vtable ptr).

```
CURSOR_VTABLE_RVA = 0x2b29a90    // CS::WorldMapCursorControl vtable RVA (imagebase 0x140000000)
OFF_X = 0xFC, OFF_Z = 0x104, OFF_Y = 0x10C    // floats on the cursor object
CURSOR_OFF_IN_MENU = 0x2DB0      // cursor lives at worldmapmenu + 0x2DB0
```

Confirmed by the probe (`.hpp:7`): the cursor sits at **worldmapmenu+0x2DB0**, coords at
**+0xFC / +0x104 / +0x10C**, and crucially **those coords are in MAP/MARKER space — the SAME
space as `WORLD_MAP_POINT_PARAM_ST.posX/posZ`** (sane observed range ~3888..6762). So **no
chunk→world bridge is needed if we drive the map in cursor space.** The probe also one-shot
dumps every float in `cursor+0xE0 .. +0x340` to locate the bounds rect (minX/minZ/maxX/maxZ,
doc says `cursor+0xF0..0x340`) → the cursor coord-space extent.

The risky live call `FUN_140a832a0` (a map-refresh) is explicitly **deferred** (`.hpp:15`) —
don't rely on it. See also `docs/windows_csworldmapmenu_re_prompt.md` (Goal 2) and
`docs/world_map_live_refresh_re.md` (anchor string VAs `0x142b48c1c`, `0x142bb5d84`,
`0x142bb5db4`; subsystem class names).

### The ask (Target B)
1. **Menu singleton → cursor chain (read):** the **getter/singleton** for `CSWorldMapMenu`
   (or the MenuMan slot that holds it) and the **pointer chain** to the cursor object at
   `+0x2DB0`, resolvable by AOB (not the scan-the-heap fallback the probe uses now). The repo
   already finds a CSMenuMan slot: AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` `{{3,7}}`
   (with `+0xCD == 7` meaning "map open") — confirm whether the world-map menu hangs off it.
2. **Is-map-open / map-ready guard:** the cleanest flag/pointer to know the world map is open
   and the cursor object is live (so we don't write into a stale/null object).
3. **THE KEY ONE — focus/move the cursor programmatically:** find the function (or the writable
   fields) that **sets the map cursor position / centres the map view** on a given map-space
   `(x, z)`. Two acceptable forms:
   - (a) a **setter function** `void(cursor*, float x, float z)` (or via a Vec2/Vec3) — give its
     AOB + signature + calling convention, and any "commit/refresh" call needed for the view to
     actually pan; **or**
   - (b) the **writable cursor fields** at `+0xFC/+0x104/+0x10C` are sufficient to move the
     reticle, plus whatever field/call makes the **camera pan** to follow it (the probe only
     proved these are *readable*; confirm they're *writable* and what triggers the view to track).
   Cross-ref params `worldMapCursorSpeed` / `worldMapCursorSnapRadius` if they gate snapping.
4. **(Optional) drop/highlight a pin** at a coord: if there's a cheap way to place the player's
   own map stamp/beacon at `(x,z)` programmatically, note the function — else we'll just move the
   cursor. (The repo's marker dumper reads beacons/stamps via `obj1+0x118`/`+0x1B8`; a writer is
   not yet known.)

### Deliverable (Target B)
- **Singleton getter AOB** + `{{first,second}}` and the **offset chain** to the cursor object.
- The **map-open guard** (flag id or pointer+offset+expected value).
- The **cursor-focus mechanism**: either the setter's **AOB + signature**, or confirmation that
  `+0xFC/+0x104/+0x10C` are writable + the **pan/commit trigger** (AOB or field).
- A verification recipe: "write `(X,Z)` of a known site of grace → map view centres there."
- Confirm the cursor coord space == `WORLD_MAP_POINT_PARAM_ST.posX/posZ` (the probe says yes;
  re-confirm with one marker so we can map a step's region → cursor coord directly).

---

## 6. How to return results

For each deliverable give, in plain text:
- **AOBs** as space-separated hex with `??` wildcards, plus the `{{first,second}}` pair if it
  resolves a RIP-relative slot, and one example resolved VA (note the build it came from).
- **Offset chains** as `[[[BASE]+a]+b]+c = meaning (type)`, each hop annotated.
- **Function signatures** as C: return type, args, calling convention (x64 fastcall:
  rcx,rdx,r8,r9 then stack), and which register/arg is what.
- **Verification samples** so the Linux side can sanity-check without the game.

Prefer **AOB + offset** over RVAs. If you can only get an RVA, still give the surrounding bytes
so we can derive an AOB.

---

## Appendix — known AOBs already in this DLL (reference / don't re-derive)

| AOB | Resolves | Source |
|---|---|---|
| `48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 90` `{{3,7}}` | ParamList static (param table root) | `from/params.cpp:15` |
| `48 8B FA 0F 11 41 70 48 8B 05` (disp@+0xA, end+0xE) | WorldChrMan static | `goblin_inject.cpp:539` |
| `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` `{{3,7}}` | CSMenuMan singleton slot (`+0xCD`==7 = map open) | `goblin_inject.cpp:1842/2066` |
| `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00` `{{3,7}}` | (another singleton slot) | `goblin_inject.cpp:2070` |
| `48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9` | ShowTutorialPopup trampoline `void(int id)` | `goblin_inject.cpp:1807` |
| `48 83 EC 28 8B 12 85 D2` | `IsEventFlag(EventFlagMan*, uint32_t*)->bool` (the flag READER) | `goblin_markers.cpp:77` |
| `48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9` `{{3,7}}` | EventFlagMan static slot | `goblin_markers.cpp:85` |
| `48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 8B 12 48 8B F1 85 D2 0F 84` | SetEventFlag (the flag WRITER, "EventFlag_C1") `u64(EventFlagMan*, u32* id, u8 val, u64 pad)` | `goblin_debug_events.cpp:175` |
| `48 8B 0D ?? ?? ?? ?? 48 8B 49 30 48 8D 55 5F` `{{3,7}}` | marker chain root slot (was RVA 0x3D5DF38) | `goblin_markers.cpp:167` |
| `48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 5F 10 48 8D 05 ?? ?? ?? ??` `{{3,7}}` | marker container vtable (was RVA 0x2AC21D8) | `goblin_markers.cpp:172` |

### Key files to read in the repo
- `src/goblin_worldmap_probe.cpp` / `.hpp` — cursor probe (offsets, menu+0x2DB0, vtable RVA).
- `src/goblin_inject.cpp:525-616` — WorldChrMan reader + both candidate pos chains.
- `docs/windows_csworldmapmenu_re_prompt.md`, `docs/world_map_live_refresh_re.md` — prior asks,
  anchor string VAs, subsystem class names.
- `src/from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp` — 256-byte marker row (areaNo 0x20, posX 0x24).
- `src/modutils.cpp` / `.hpp` — Pattern16 scan + `{{first,second}}` RIP-relative convention.
