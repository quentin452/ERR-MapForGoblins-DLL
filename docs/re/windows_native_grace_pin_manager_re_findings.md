# RE findings — the native DISCOVERED-GRACE map-pin system (it's WarpPinData, not +0x398)

Answers `docs/re/windows_native_grace_pin_manager_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
scripts `re_v92..v95`). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.

> **Scope split (as flagged):** the **static** side — the pin SYSTEM, its build path, and the
> discovered gate — is fully delivered below. The **live container address** + the
> find-what-accesses validation are the runtime job the brief calls for (pointer-scan from a drawn
> pin / the dialog's WarpList); §3/§7 give the exact targets to point your probe at.

---

## 0. TL;DR

The discovered-grace pins are **`CS::WorldMapWarpPinData`** (the bonfire/warp pin type) — a system
**entirely separate** from `CSWorldMapPointIns`/`+0x398` (correctly proven empty). Graces are sites of
grace = bonfire warps, so they live in the **warp** pin list, not the point list.

- **#1 system / container** → `CS::WorldMapWarpPinData` (vtable `0x2ad8228`) held in
  `CS::WorldMapPinDataList<CS::WorldMapWarpPinData>` (vtable `0x2ad82a8`); sourced from
  `CS::WorldMapWarpData` (vtable `0x2ad8840`, 0x38-byte items) in `CS::WorldMapWarpDataList`
  (`MenuViewItemList<WorldMapWarpData>`, vtable `0x2ad8898`). Both hang off the
  `WorldMapViewModel`/dialog (one of its `WorldMapItemControl` lists = the **WarpList**). §1.
- **#2 discovered gate** → each `WarpData` wraps a source entry at **`warpData+0x8`**; the
  **state/discovered flags are `[warpData+0x8]+0x1E` (byte, bits 0/1/2)** (`FUN_140d25540`), and the
  **iconId is `[warpData+0x8]+0x08`** (`FUN_140d25650`). A grace becomes a drawn pin per those bits.
  §2.
- **#3 suppression** → the WarpPinData list is **grace/warp-specific** (separate from player dot, fog,
  objectives, point-markers) → filtering/clearing it suppresses only graces. Hook the pin builder
  `FUN_14088b7b0` to skip discovered ones, or clear the list via its own method. §3.
- **#4 sibling/player** → `[er+0x3D6F558]` and `[er+0x3D69BA8]` are NOT this system (player-pos /
  point subsystem); the grace pins are the Warp list above. §4.
- **§6 sprite** → the discovered-grace sprite is `SB_ERR_Grace_Morning_Color` (ERR gfx symbol,
  already live-captured), carried by the **world-map movie's** image-list → **§8-walkable** (hook
  `FUN_140d69640` on the world-map movie, not the inventory movie). §6.

---

## 1. The grace-pin class system (#1, static)

```
CS::WorldMapWarpData          vtable 0x2ad8840   // 0x38-byte item; warpData+0x8 → source entry
CS::WorldMapWarpDataList      vtable 0x2ad8898   // MenuViewItemList<WorldMapWarpData> (vector +1/+2/+3 = begin/end/cap, stride 0x38)
CS::WorldMapWarpPinData       vtable 0x2ad8228   // the DRAWN grace/warp pin (built from a WarpData)
CS::WorldMapPinDataList<WorldMapWarpPinData>  vtable 0x2ad82a8   // the pin container
CS::MenuViewItemList<WorldMapWarpData>        vtable 0x2ad8860
CS::WorldMapWarpSelectDialog  vtable 0x2b31f68   // the fast-travel list dialog (same source)
```
Sibling pin types (the 4 `WorldMapPinDataBase 0x2ad5d30` subclasses): `WorldMapPointPinData`
(`0x2ad6688`), `WorldMapSignPuddlePinData` (`0x2ad6f28`), `WorldMapPinData` (`0x2ad6608`), and
**`WorldMapWarpPinData`** (the graces). So the map dialog keeps *separate* lists per pin kind —
suppressing the Warp list touches graces only.

**Build path:**
- `FUN_14088b060` (`0x88b060`) — builds the `WorldMapWarpDataList`: `src = FUN_14088b3e0()` (the
  source bonfire/grace data array) → `FUN_14088a0f0` copy-constructs `WorldMapWarpData` items (stride
  `0x38`) into the list (`list+1/+2/+3` = begin/end/cap).
- `FUN_14088b7b0` (`0x88b7b0`) — builds a `WorldMapWarpPinData` **from** a `WarpData` (`param_3`): sets
  `WorldMapPinData`→`WorldMapWarpPinData` vtables, copies the MapId/pos block (`param_3[0..3]` →
  `pin+0x47…`), reads the **state byte** (`FUN_140d25540`→`pin+0xC`) and **iconId**
  (`FUN_140d25650`→`pin+0xA`), and the area/pos ushorts (`[warpData+8]+0x10/+0x1c/+0xe8/+0xea`).
- Setup/driver: `FUN_14088a6c0` (`0x88a6c0`, 1235B) / `FUN_14088aba0` (`0x88aba0`, 680B) — the
  `WorldMapViewModel`/dialog warp-list setup (callers of `FUN_14088b060`).

## 2. The discovered gate (#2)

Each `WarpData` points at a source entry at **`warpData+0x8`** (the bonfire/grace registry record):
```c
// FUN_140d25540 — state/visibility:
char state(WarpData* w){ if (!*(void**)(w+8)) return 0;
    byte b = *(byte*)(*(void**)(w+8) + 0x1E);  return (b&1)+(b&2)+(b&4); }   // bits 0/1/2
// FUN_140d25650 — iconId:
int iconId(WarpData* w){ if (!*(void**)(w+8)) return 0;
    int v = *(int*)(*(void**)(w+8) + 0x08); return v==-1 ? 0 : v; }
```
So **`[warpData+0x8]+0x1E` byte bits 0/1/2 = the registered/discovered/visible flags** that promote a
grace into a drawn pin, and `[warpData+0x8]+0x08` = its iconId, `[warpData+0x8]+0x18` = its id. This
is the bonfire "discovered/activated" gate the brief asked for — it's a **flag on the source record**
(fed from the bonfire DB `CS::CSNetBonfireDb`/`BonfireList` + `BonfireWarpParam`), not a per-frame
event-flag read in the pin code. (`BonfireWarpParam` string `0x2bb2c78` is fetched by param index,
not by name — 0 exe xrefs, as expected.)

## 3. Suppression recipe (#3) — grace-only, keeps player/fog/objectives

Because graces are their **own** pin list (`WorldMapPinDataList<WorldMapWarpPinData>`), suppressing it
does **not** touch the player "you are here" dot, fog/fragments, point-markers, or objective beacons
(those are other subsystems / other pin lists). Options, safest first:
1. **Filter at build (non-destructive):** hook `FUN_14088b7b0` (the WarpPinData builder) and, for a
   grace whose `state()` byte (§2) marks it discovered, skip adding it / mark it hidden. Lets the
   overlay draw it instead. No freeing, no hand-walking a container.
2. **Clear the list via its own clear:** call the `WorldMapPinDataList<WarpPinData>`'s clear method
   (`list` vtable `0x2ad82a8`) rather than freeing nodes by hand. Re-clear when the list rebuilds.
3. Avoid hand-walking/freeing the vector (the brief's `[PINSET]` crash) — use #1 or #2.
All must be RPM/WPM-safe with no `__try` reliance (the clang-cl SEH elision note); a build-time skip
(#1) is the least crash-prone since it runs on the engine's own thread during the rebuild.

## 4. Sibling / player-mgr clarification (#4)

- `[er+0x3D6E9B0]+0x398` = `CSWorldMapPointIns` map — **empty** (no injected WorldMapPointParam rows);
  NOT the graces.
- `[er+0x3D6F558]` (`FUN_140aba1a0`) and `[er+0x3D69BA8]` (player-pos mgr, `+0x70`/`+0x74` block-local
  X/Z) are the **point/player** subsystems — KEEP them. The grace pins are the **Warp** list (§1),
  reached off the `WorldMapViewModel`/dialog, not these slots.

## 5. The live container — runtime step (the brief's #1 validation)

The `WorldMapWarpDataList` / `WorldMapPinDataList<WarpPinData>` instances are dialog/VM fields (one of
the dialog's `WorldMapItemControl` lists). To pin the live address: with N graces discovered, **scan
the open `WorldMapDialog` (cursor−0x2DB0) / `WorldMapViewModel` (vtable `0x2ad82e0`) for a vector
(begin/end/cap, stride 0x38 for WarpData) of size N**, or **find-what-accesses** the `FUN_140d25540`
state read while panning. Validate: N entries ↔ N discovered graces, each `iconId`/id matching the
expected bonfire. Then point `dump_native_pins` at that list (not `+0x398`).

## 6. §6 — the discovered-grace sprite (harvestable via §8)

`SB_ERR_Grace` is **not** in the exe (ERR-gfx symbol; not a vanilla string — `re_v92` found none).
It was already **live-captured**: `SB_ERR_Grace_Morning_Color` at rect (818,368)-(892,442), 74×74, on
the 2048×1024 ERR sheet (`windows_runtime_icon_textures_followup_re_findings.md`). The `WarpPinData`
selects its sprite by `iconId` (§2) → a GFx symbol in the **world-map movie's** image-list. So **yes,
it's §8-walkable** — hook `FUN_140d69640` on the **world-map** movie (the warp pins draw through the
same Scaleform image system as item icons, just a different movie) and harvest `SB_ERR_Grace_*` (lit)
the same way as `MENU_*` icons. Combined with §1-§3 suppression, the overlay can then draw both the
inert (iconId-370) and the **lit/discovered** grace and become the sole grace source.

## 7. Handles

- classes/vtables: §1. `WarpData` 0x38B; source entry `warpData+0x8` (`+0x08` iconId, `+0x18` id,
  `+0x1E` flags bits0/1/2, `+0x10/+0x1c/+0xe8/+0xea` area/pos ushorts).
- state/discovered `FUN_140d25540` `0xd25540` (`[w+8]+0x1E & 7`); iconId `FUN_140d25650` `0xd25650`
  (`[w+8]+0x08`).
- builders: WarpDataList `FUN_14088b060` `0x88b060` (source `FUN_14088b3e0`); copy-ctor
  `FUN_14088a0f0` `0x88a0f0`; WarpPinData builder `FUN_14088b7b0` `0x88b7b0`; setup `FUN_14088a6c0`
  `0x88a6c0` / `FUN_14088aba0` `0x88aba0`.
- §6 sprite `SB_ERR_Grace_Morning_Color` (world-map movie image-list, §8 walk via `FUN_140d69640`).
- Resolve fns by AOB, the lists by VM/dialog scan + vtable (`0x2ad82a8`/`0x2ad8898`) — RVAs drift.
- Runtime: pin the live WarpPinData list (§5), point `dump_native_pins` at it, validate N↔N.
```
