# RE brief — find the native DISCOVERED-GRACE map-pin system (the empty +0x398 manager is the wrong target)

App 2.6.2.0 / ERR 2.2.9.6. ERSC + ERR loaded; our DLL (MapForGoblins) is in-process.
Our overlay now runs in **overlay-sole mode** (`native_map_injection=false`): we draw all
markers ourselves via ImGui/DX12, the engine no longer injects our WorldMapPointParam rows.

We want the overlay to become the **sole** source of grace map-pins too. Today it's a HYBRID:
- **undiscovered** graces → drawn by our overlay (ImGui).
- **discovered** graces → drawn by the **native game** (eldenring.exe), because our overlay
  lacks the discovered/rested-grace sprite (separate RE, see §6).

To suppress the native discovered-grace pins we must find **where they live + how they're drawn**.
The previous suppression RE pointed at `CSWorldMapPointMan = [er_base+0x3D6E9B0]`, std::map
`<int,CSWorldMapPointIns*>` at `+0x398`. **We have now PROVEN at runtime that this manager is
EMPTY in overlay-sole mode** — so it is NOT where the grace pins are. We need the real system.

---

## Decisive runtime evidence (our `dump_native_pins` probe, config `dump_native_pins`)

We resolve `er_base = GetModuleHandle("eldenring.exe")` (ASLR-correct; other readers use it fine).
With the world map OPEN, discovered-grace pins VISIBLY drawn, and while PANNING, we walked the
manager with the corrected MSVC `std::map` offsets (`_Myhead@+0x398`, `_Mysize@+0x3A0`,
node key@+0x20 / value@+0x28 — per `windows_suppress_native_pins_runtime_re_findings.md`):

```
[PINS] PRIMARY[er+0x3D6E9B0] mgr=0xfd735400 size=0 head=0xfd8af780   (valid empty std::map)
[PINS] PRIMARY[er+0x3D6E9B0] mgr=0xfd735400 size=0 head=0xfd8af780   (still 0, 0.4s later, panning)
[PINS] SIBLING[er+0x3D6F558] mgr=0xfd775a80 size=1057757935 head=0xbf56422f  (garbage → not a std::map at these offsets)
```

- `mgr` is a live heap pointer that varies across runs (real object), with a valid sentinel
  `head` close to it and `_Mysize == 0`. So `[er+0x3D6E9B0]+0x398` is a **genuinely empty**
  std::map — consistent with "no WorldMapPointParam rows are injected" → the engine builds this
  set empty. The render walk `MOV RDI,[R15+0x398]` (reconcile `FUN_140a832a0`, `er+0xa8397e`)
  therefore draws nothing from here.
- **The discovered-grace pins are drawn anyway** → they come from a **different** system.
- `SIBLING [er+0x3D6F558]` (caller `FUN_140623410` also touches it via `FUN_140aba1a0`,
  `er+0x62426f`) reads garbage at `+0x398/+0x3A0` → either wrong offsets for that manager or it's
  a different object shape. Untested beyond this.

Background (our code, `goblin_inject.cpp:911`): *"vanilla WorldMapPointParam does NOT contain grace
markers — in the base game grace map-pins are generated from **BonfireWarpParam** at runtime, not
stored here."* So the native grace pins are a **bonfire/grace → map-pin** path, not WorldMapPointParam.

---

## What we need (runtime-validated, NOT a static guess — the last static guess failed)

1. **The native grace-pin manager/container.** Where does the game store the discovered-grace
   map-pins it draws on the open world map? Resolve it the way player-pos was cracked: a runtime
   **find-what-accesses** on the grace-pin draw, or a **pointer-scan** from a visibly-drawn grace
   pin back to its owning container. Report: the static slot (`er_base + RVA`), the deref chain,
   the container type/shape, and the per-entry layout (id/area/pos/iconId/discovered-flag fields).
   Validate live: with N graces discovered, the container holds N entries; entries map to the
   right bonfire/grace ids.

2. **How a grace enters it (the discovered gate).** Confirm the per-grace **event flag** (bonfire
   "discovered/activated") that promotes a BonfireWarpParam row into a drawn map-pin. We need the
   flag→pin relationship so suppression and our own draw stay in sync.

3. **A safe suppression recipe.** Once the container is known, the recipe to stop the native
   discovered-grace pins from drawing **without** touching: player "you are here" dot, fog/map
   fragments, player-placed markers, objective beacons. Prefer a non-destructive filter (a
   show-predicate / per-entry skip) over freeing nodes; if clearing a container, use the game's
   own clear, not a hand walk. Must be ReadProcessMemory/WriteProcessMemory-safe (no `__try` — the
   compiler elides it here, see the project's clang-cl SEH note; our `[PINSET]` probe crashed that way).

4. **(Sibling sanity)** Re-resolve `[er+0x3D6F558]` (`FUN_140aba1a0`) and the player-pos manager
   `[er+0x3D69BA8]` correctly (right offsets) and report which one — if any — owns the grace/bonfire
   pins vs the player-marker/objective system we must KEEP.

## §6 — bonus that unblocks the OTHER half (so we can draw discovered graces ourselves)

To make the overlay the sole grace source we also need the **discovered/rested grace SPRITE**
(we only have the inert iconId-370 sprite). The runtime item-icon harvest already cracked the
GFx resident-image walk: `windows_runtime_item_sprite_re_findings.md` §8 (commit `6913ec4`) —
`[movie+0x40]` gate `+0x88==4` → `+0x90` = image-list, `list+0x78` count / `list+0x80` arr
(stride 8), entry name DLWString `+0x18` (heap iff `*(entry+0x30) >= 8`). Names are GFx symbols
(`MENU_ItemIcon_<id>` / `MENU_FL_<id>` / `KG_*` / `SB_ERR_*`).

**Ask:** does the **world-map movie** expose the grace pin sprite(s) the same way (a GFx symbol for
the discovered/rested site-of-grace icon)? If so, report the symbol name(s) + which movie's
image-list carries them, so we can harvest+draw them with the existing §8 mechanism. Then native
suppression (§1-§3) + our discovered-grace draw together make the overlay the sole grace source.

## Handles / leads
- `er_base = 0x140000000` (imagebase); resolve live via `GetModuleHandle("eldenring.exe")`.
- empty manager `[er+0x3D6E9B0]`; reconcile `FUN_140a832a0` (walk `[R15+0x398]` @ `er+0xa8397e`);
  caller `FUN_140623410` (also `[er+0x3D6F558]` via `FUN_140aba1a0` @ `er+0x62426f`, and player-pos
  mgr `[er+0x3D69BA8]`).
- grace source = **BonfireWarpParam** (id → MSB position; the discovered gate is a bonfire event flag).
- our probe to extend: `goblin_worldmap_probe.cpp` `dump_native_pins` / `dump_native_pin_map`
  (ReadProcessMemory walk, config `dump_native_pins`) — point it at the real manager once found.
- world-map dialog/cursor handles (for movie/GFx access): cursor = `WorldMapDialog + 0x2DB0`;
  VM vtable `0x2ad82e0`. Item-icon movie captured via the ENUM hook on `FUN_140d69640`.

## Deliverables
A findings doc `windows_native_grace_pin_manager_re_findings.md` with: the grace-pin manager
static+chain+offsets (runtime-validated), entry layout, the discovered-flag relationship, the safe
suppression recipe (keep player/fog/objectives), the sibling/player-mgr clarification, and (§6) the
discovered-grace GFx sprite symbol(s) + movie if exposed. RVAs may drift under VMProtect — give the
stable contract (chain shape + offsets) and resolve fns/objects by AOB / vtable-scan / pointer-scan.
