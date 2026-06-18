# Task: RE the world-map icon rebuild fn for live section-toggle refresh (Windows)

You are on **Windows** with **Ghidra or IDA** and a **debugger that can attach to the
running game** (Cheat Engine and/or x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden
Ring world-map icon mod, DLL injected via Mod Engine / ERSC). Branch for this work:
`feat/section-toggle` (the in-game per-section toggle already shipped there).

## Background / what's already done

The mod adds an **in-game per-section visibility toggle** (7 display groups:
Equipment, Key Items, Loot, Magic, Quest, Reforged, World). Hotkeys: **F8** cycles the
selected group, **F7** shows/hides it. It works by flipping each injected
`WorldMapPointParam` row's `areaNo` to 99 (hide) / back (show) on the live param table
— see `apply_section_visibility()` in `src/goblin_inject.cpp`.

**The problem you are solving:** the flip only takes effect on the **next map open**.
The game builds the world-map icon set **once** per map-open (on the CSMenuMan
`0 → 3` loading edge) and **never re-reads `WorldMapPointParam` while the map is open**
(documented in `docs/ersc_hosting_and_map_autohide.md`). We want the toggle to refresh
icons **live, while the map is open**, with no reopen.

Full findings + rationale: `docs/world_map_live_refresh_re.md`. Read it first.

## Your objective

Find the game routine that **(re)builds the world-map icon set from
`WorldMapPointParam`**, resolve it by a patch-resilient AOB, and confirm calling it
after our flip refreshes icons live without crashing. Then we wire it into the DLL.

## Subsystem map (from eldenring.exe cleartext RTTI)

Game exe: `…/steamapps/common/ELDEN RING/Game/eldenring.exe` (VMProtect-packed: there
is a 2nd `.text` at VA `0x144c0e000` — analyse the real MSVC `.text` at `0x140001000`,
not the VMP one).

- **`CS::CSWorldMapPointMan` / `CSWorldMapPointManImplement`** — `FD4Singleton` that
  owns the placed map-point instances. The rebuild/create-all method is almost
  certainly a method here. **Primary target.**
- `CS::CSWorldMapPointManImplement::_DiscoverMapPoint` — reveals ONE point (grace /
  fragment discovery). Per-point, not a full rebuild — but a useful xref neighbour.
- `CSWorldMapPointIns`, `CSWorldMapDiscoveryPointIns`, `CSWorldMapReentryPointIns` —
  the per-icon instances built from `WorldMapPointParam` rows.
- `CS::WorldMapPointParam::_VerifyEnableEventFlag` / `_VerifyDisableEventFlag` —
  per-row visibility evaluators (gate an icon on its enable/disable event flag).
- `CS::MapItemManImpl` — related map item/icon manager.

### Anchor string VAs (xref starting points)
Computed `rdata_VA(0x1429af000) + (file_off − rdata_file_off(0x029ae200))`:

| String | VA |
|---|---|
| `CSWorldMapPointManImplement::_DiscoverMapPoint` | `0x142b48c1c` |
| `WorldMapPointParam::_VerifyEnableEventFlag`     | `0x142bb5d84` |
| `WorldMapPointParam::_VerifyDisableEventFlag`    | `0x142bb5db4` |

These are scope/name strings; the function that LEA-references one is (or sits at) that
routine. This is exactly how the mod pins `ShowTutorialPopup` — see the comment in
`src/goblin_inject.cpp` ("LEA xref to `CS::CSPopupMenu::_CanOpenTutorialParam`").

## Steps

1. **Static (Ghidra/IDA).** Import the exe, auto-analyse. Go to each anchor string,
   follow xrefs. Identify the `CSWorldMapPointManImplement` method that **iterates
   `WorldMapPointParam` and (re)creates the `CSWorldMapPointIns` set** — i.e. the
   map-open build routine (look near where it reads the param table, news up
   `CSWorldMapPointIns`, and where `_Verify*EventFlag` is called from).
2. **Find the on-demand entry.** Either a public "rebuild/refresh/reset" method on the
   singleton, or the exact build call made on map-open that can be re-driven while
   open (CSMenuMan `+0xCD == 7`).
3. **Runtime confirm (debugger).** Attach to the running game, open the map, and call
   the candidate on the live `CSWorldMapPointMan` singleton (script it / breakpoint +
   set RIP, or a tiny test patch). Verify: icons re-read the current param (toggle a
   row's `areaNo` first, then call → icon should appear/vanish) and **no crash**.
4. **Resolve by AOB, not RVA.** Pin the function and the singleton getter/slot with a
   stable surrounding-byte signature (RVAs shift every game patch — the mod already
   re-resolves everything by AOB at runtime).

## Deliverables

1. The **AOB signature(s)**: the rebuild method, and how to obtain the
   `CSWorldMapPointMan` singleton (FD4Singleton getter or static slot AOB +
   RIP-relative offsets), plus the resolved RVAs for the current build for reference.
2. The **calling convention + signature** of the rebuild method (args, `this`, return),
   and any preconditions (must be called on the main/render thread? only when map
   state == 7? needs a specific sub-map/page arg?).
3. A **minimal C++ snippet** matching the mod's style (`modutils::scan<>` AOB resolve,
   cached, SEH-guarded call) for a `goblin::refresh_world_map_icons()` we can drop into
   `goblin_inject.cpp` and call from `menu_auto_toggle_loop` right after
   `apply_section_visibility()` when the map is open.
4. **Runtime evidence**: a short note / screenshot that calling it live refreshes the
   icons with no reopen and no crash (test with a section toggle).

If no clean rebuild method exists, report that with the evidence and fall back: find the
**map menu close+open** entry instead (programmatic reopen, 1-frame flicker) and deliver
its AOB the same way. State clearly which path you delivered.
