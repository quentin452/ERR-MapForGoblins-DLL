# World-map RE findings (Windows / Ghidra) — section-toggle live refresh + map cursor

Static RE of `eldenring.exe` (Steam, **app build analysed 2026-06**) for the two goals in
`docs/windows_csworldmapmenu_re_prompt.md` (extends `windows_live_refresh_re_prompt.md`).
Done with **Ghidra 12.1.2** headless. RVAs are relative to imagebase `0x140000000`; the real
MSVC `.text` is `0x140001000` (the VMProtect `.text` at `0x144c0e000` was ignored — it only
produced the expected "Failed to create function … referring thunk" noise).

> **Status:** static anchors below are solid (resolved by RTTI walk + decompiler). The two
> items that need the **running game** to finish (and which you said you'd do) are flagged
> **[RUNTIME]**: (1) confirm which routine actually re-renders icons live, (2) confirm the
> cursor X/Z offset order + the menu→cursor pointer. Resolve every address by AOB before
> wiring (RVAs shift each patch).

## How to re-run the analysis (project persists, no re-analyse)
The 2 h auto-analysis is saved in `D:\ghidra_proj2\ER` (1.4 GB). Re-run any script in minutes:
```
GHIDRA_HEADLESS_MAXMEM=16G analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe ^
   -noanalysis -postScript <script>.java -scriptPath D:\ghidra_scripts
```
(Scripts must be **Java** GhidraScripts — Ghidra 12 dropped Jython; Python needs PyGhidra.)
Recon scripts used: `find_worldmap.java`, `re_v2…re_v7.java` in `D:\ghidra_scripts`.

---

## GOAL 1 — live icon refresh

### Solid anchors
| What | RVA | VA | note |
|---|---|---|---|
| **`CSWorldMapPointMan` FD4Singleton instance slot** | `0x3d6e9d8` | `0x143d6e9d8` | `mov rax,[rip+0x3cccd3d]` |
| singleton registration (sets name, ctor-on-null) | `0xa1c90` | `FUN_1400a1c90` | reads the slot |
| singleton ctor (alloc) | `0xa84420` | `FUN_140a84420` | called when slot==0 |
| **`CSWorldMapPointIns` vtable** | `0x2b487a8` | `0x142b487a8` | the per-icon instance |
| `CSWorldMapPointIns` ctor(s) | `0xa811e0`, `0xa812d0` | — | install the vtable |
| `_DiscoverMapPoint` (reveal ONE point) | `0xa84080` | `FUN_140a84080` | grace/fragment discovery |
| per-row visibility predicate | `0xd58470` | `FUN_140d58470` | calls both `_Verify*` |
| `_VerifyEnableEventFlag` | `0xd58640` | `FUN_140d58640` | render-time gate |
| `_VerifyDisableEventFlag` | `0xd58550` | `FUN_140d58550` | render-time gate |

**Singleton getter AOB** (resolves the slot from the `mov rax,[rip+disp]`; disp at +3 gives the
slot, slot = `hit + 7 + *(i32*)(hit+3)`):
```
48 8B 05 ?? ?? ?? ?? 48 85 C0 75 05 E8     ; mov rax,[rip+x]; test rax,rax; jnz +5; call ctor
```
(entry of `FUN_1400a1c90`: `48 83 EC 28 48 8B 05 3D CD CC 03 48 85 C0 75 05 E8 …`)

### The rebuild / re-filter routine — top candidate **[RUNTIME to confirm]**
The `CSWorldMapPointIns` ctors have **no direct callers** (instances are new'd via an indirect
factory), so the "create-all on map-open" couldn't be pinned by call-graph. But the manager
region `0xa8xxxx` (`CSWorldMapPointManImplement`) has one dominant routine:

- **`FUN_140a832a0`** (RVA `0xa832a0`, **2470 bytes** — by far the biggest in the region).
  It **iterates the manager's point list** (`**(param_1+8)`, walking linked nodes) and, per
  node, evaluates event flags via `FUN_140d09bf0(<flagMan singleton 0x143d7d478>, <flagId>, 1)`
  **and calls `_DiscoverMapPoint` (`FUN_140a84080`)** + per-point ops (`FUN_140a844d0`,
  `FUN_140a84210`, `FUN_140a816b0`, `FUN_140a81530/81560`). So it is the **per-point
  refresh / re-evaluation loop** over the *existing* point set (it does **not** new up
  `CSWorldMapPointIns` — it's the refresh layer, not the initial builder). That makes it the
  best live-refresh entry to try. `this`(param_1) = the `CSWorldMapPointMan` singleton instance
  (slot above); `param_2` = the current map/sub-page context.
  AOB(entry): `40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 …`

  Other manager-region candidates if `0xa832a0` is the wrong layer: `FUN_140a850c0` (985 B),
  `FUN_140a82eb0` (717 B), `FUN_140a81890` (694 B).

**[RUNTIME] confirm:** attach (Cheat Engine), open the world map, flip a row's `areaNo` (as the
mod does), then call `FUN_140a832a0(singleton, <ctx>)` on the main thread (script a call / set
RIP). If icons update with no reopen and no crash → that's the refresh entry. The caveat is that
this routine appears to gate on **event flags**, not `areaNo`; if our `areaNo`-99 flip is read
only at the *build* layer (point creation), a flag-only re-filter won't see it → then fall back
(below).

### C++ skeleton (mod style) — once the routine is confirmed
```cpp
// resolve once, cached, SEH-guarded — modutils::scan<> like the existing trampolines
static void* (*pRefresh)(void* mgr, void* ctx) = nullptr;
static void** pPointManSlot = nullptr;   // -> CSWorldMapPointMan instance

void goblin::refresh_world_map_icons() {
    if (!pRefresh || !pPointManSlot) return;
    void* mgr = *pPointManSlot;
    if (!mgr) return;
    __try { pRefresh(mgr, /*ctx*/ nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// call from menu_auto_toggle_loop right after apply_section_visibility() when CSMenuMan+0xCD==7
```

### Fallback (if no flag-independent rebuild exists) — **programmatic reopen**
Re-drive the map-menu close+open via `CSMenuMan` (1-frame flicker). The menu world-map setup
routine (which also builds the cursor, see Goal 2) is **`FUN_1409be5e0`** (RVA `0x9be5e0`); the
open/close entry is reachable from `CSMenuMan` (`+0xCD` state byte, already AOB'd by the mod).
Deliver its AOB the same way. State this path clearly if used.

---

## GOAL 2 — map cursor position  (essentially mapped statically)

### Solid anchors
| What | RVA | VA |
|---|---|---|
| **`CS::WorldMapCursorControl` vtable** | `0x2b29a90` | `0x142b29a90` |
| cursor ctor (field init) | `0x9bc5b0` | `FUN_1409bc5b0` |
| cursor **Update/Move** method (vtable[2]) | `0x9bd4b0` | `FUN_1409bd4b0` |
| cursor **owner** = world-map menu setup | `0x9be5e0` | `FUN_1409be5e0` (creates the cursor) |

### Cursor position layout (from the Update method `FUN_1409bd4b0`)
Inside a `WorldMapCursorControl` instance:
- **position fields at `+0xFC`, `+0x104`, `+0x10C`** (read together in the Update method; ctor
  inits all three to a default float). These are the cursor coordinates.
- `+0xF0` → a sub-object whose **bounds rect** is at `+0x340 / +0x344 / +0x348 / +0x34C`
  (minX / minZ / maxX / maxZ). The Update clamps the cursor to this rect → **this is map/marker
  space**, the same space as `WorldMapPointParam` posX/posZ + grid (exactly what we want).
- `+0x90` → a vtabled sub-object (camera/projection), `+0x130` holds `0.1f` (a lerp/speed const).
- The cursor object also embeds a `CS::CSMenuPositionComponent` (at `+0x10`) and a
  `CSMenuVisibleComponent`.

**[RUNTIME] confirm:** with CE, read floats at `cursor+0xFC/+0x104/+0x10C` while moving the
reticle; identify which is X and which is Z (the third is likely Y/zoom). Cross-check against a
known marker's posX/posZ + (gridXNo,gridZNo) using the tile math in
`src/goblin/goblin_map_tiles.hpp` (`XX=gridXNo/2`, marker posX/posZ tile-local ±~500).

### Menu → cursor offset — **FOUND (embedded sub-object)**
The cursor is **embedded inside the world-map menu**, not pointer-linked. In the menu setup
`FUN_1409be5e0` the cursor is constructed in place:
```
FUN_1409bc5b0(param_1 + 0x5b6, ..., param_1 + 0x4fb);   // param_1 = menu (undefined8*)
```
`param_1 + 0x5b6` (undefined8* arithmetic) = **menu + 0x2DB0 bytes** = the `WorldMapCursorControl`
sub-object. So the full chain is:
```
cursor_pos_X = *(float*)( menu + 0x2DB0 + 0xFC )      // +0x104 / +0x10C = the other two coords
```
**Only remaining unknown = the world-map-menu instance pointer** (`menu`). Get it at runtime
(scan for the `WorldMapCursorControl` vtable `0x142b29a90` in memory → that object −0x2DB0 = menu;
or reach the menu via `CSMenuMan` while `+0xCD==7`), or one more static pass to find the menu
singleton getter / its `CSMenuMan` slot.

### C++ skeleton
```cpp
struct CursorPos { float x, z; };
static uintptr_t (*get_worldmap_menu)() = nullptr;   // [RUNTIME] menu singleton getter
constexpr uintptr_t CURSOR_OFF = 0x2DB0;             // cursor embedded in the menu (found)
bool goblin::get_map_cursor_pos(CursorPos& out) {
    uintptr_t menu = get_worldmap_menu ? get_worldmap_menu() : 0; if (!menu) return false;
    uintptr_t cur = menu + CURSOR_OFF;               // embedded sub-object (not a pointer)
    out.x = *(float*)(cur + 0xFC);     // confirm X vs Z at runtime
    out.z = *(float*)(cur + 0x104);    // (or 0x10C)
    return true;
}
```

---

## AOB summary (resolve by these, not RVA)
| Symbol | entry AOB (first bytes; verify uniqueness, wildcard rip-disp) |
|---|---|
| PointMan singleton getter | `48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 05 E8` |
| rebuild/re-filter cand. `0xa832a0` | `40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00` |
| `_VerifyEnableEventFlag` `0xd58640` | `48 89 5C 24 10 48 89 74 24 20 57 48 83 EC 40 33 DB 8B F2 48 8B F9 89 5C 24 20 48 39 59 10` |
| per-row predicate `0xd58470` | `48 89 74 24 10 57 48 83 EC 20 4C 8B 51 10 48 8B F1 48 63 FA 4D 85 D2` |
| cursor ctor `0x9bc5b0` | `48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 74 24 50` |
| cursor Update `0x9bd4b0` (vtable[2]) | (decompiled; entry in re_v6 output) |
| cursor owner / menu setup `0x9be5e0` | `66 44 89 4C 24 20 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 68 EA FF FF` |

## What's left (both are the **[RUNTIME]** items you'll do)
1. Confirm the live-refresh entry: test calling `FUN_140a832a0` after the `areaNo` flip; if it
   ignores `areaNo` (flag-only), use the programmatic-reopen fallback via `FUN_1409be5e0`/CSMenuMan.
2. Confirm cursor X/Z offset order at `cursor+0xFC/0x104/0x10C` and grab the menu→cursor pointer
   chain live (or via one more decompile pass of `FUN_1409be5e0`).
