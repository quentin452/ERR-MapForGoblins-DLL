# RE findings — suppress native world-map pins

Answers `docs/re/windows_suppress_native_pins_re_prompt.md`. Static Ghidra (`D:\ghidra_proj2\ER`,
scripts `find_suppresspins.java` / `find_realbuild.java` / `find_build3.java`). App 2.6.2.0 / ERR
2.2.9.6.

---

## 0. TL;DR — prefer the show-predicate filter over clearing `+0x398`

The brief's Plan B (empty `CSWorldMapPointMan+0x398` from the build hook) has two problems this RE
surfaced. There is a **safer mechanism the mod already uses**: the per-row **show-predicate runs
BEFORE insertion** into `+0x398`, so a row that fails it is **never built** — no native pin, nothing
to clear. The mod already flips `WORLD_MAP_POINT_PARAM_ST.areaNo → 99` to hide points; extend that to
**all** the category+grace rows the overlay draws and the native pins simply don't appear. No
RB-tree mutation, no new hook.

---

## 1. `CSWorldMapPointMan+0x398` — container layout (confirmed)

It is an **MSVC `std::map<int id, CSWorldMapPointIns*>`** (red-black tree), confirmed by the textbook
RB-tree walk in `FUN_140a82a80`:
- header pointer at `[mgr + 0x398]`; node layout: `+0x0` left, `+0x8` parent, `+0x10` right,
  `+0x19` color/nil byte, `+0x20` key (int id), `+0x28` value (`CS::CSWorldMapPointIns*`).
- `mgr` = `CSWorldMapPointMan` instance `[er_base + 0x3D6E9B0]` (FD4 getter slot `0x3D6E9D8` resolves
  the same object). Holds the WorldMapPointParam-derived icons (graces + category/boss/item pins).
- The render walk reads `[R15 + 0x398]` (`marker_affine_hook_re_findings.md` @ `0xa8397e`); reconcile
  is `FUN_140a832a0`.

## 2. Correction to the brief — the mod does NOT hook the build

The `WORLDMAP_POINT_CTOR` AOB
(`40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF FF FF 4C 8B F9 8B 42 34`)
resolves to **`FUN_140a82a80`**, whose body is a per-point **discover/select TOGGLE**: it walks the
`+0x398` tree and, for the entry whose id == `ctx+0x34`, calls a setter — it does **not** `new`
`CSWorldMapPointIns` and does **not** insert. So `wm_build_detour` clearing `+0x398` "after the
original build" is built on a wrong premise — that hook isn't the populate.
- The real build (the one that `new`s `CSWorldMapPointIns`, ctor `FUN_140a811e0`) is an **adjacent
  function ~`0x140a82c30`** (its ctor call site `0x140a82d09` is left UNDEFINED in the DB — VMP-/
  analysis-gap), plus the reconcile `FUN_140a832a0` (which calls `_DiscoverMapPoint FUN_140a84080`).
- Per prior RE (`windows_marker_offset_getter_capture_re_prompt.md`): the **show-predicate is vt[1]
  `FUN_140a81450`, evaluated BEFORE insert** — a hidden row is never built into `+0x398`. This is the
  hook the suppression should use.

## 3. Keep-items — which survive (separation)

| item | in `+0x398`? | system |
|---|---|---|
| grace pins (lit/unlit) | **yes** | WorldMapPointParam → CSWorldMapPointMan |
| category/boss/item pins | **yes** | same |
| fog / map-fragment reveal | **no — separate** | `WorldMapPieceParam` piece manager (fog findings: `FUN_1408890b0` builds piece objects + the `[mgr+0x39c]` reveal mask — a different manager) |
| "you are here" dot | **no — separate (likely)** | rendered from the player position `[[WCM+0x1E508]+0x6C0]` (resolved), not a WorldMapPointParam row |
| player-placed markers | **unconfirmed** | likely a separate map-entity/marker manager (needs the runtime container check below) |
| objective / guidance beacons | **unconfirmed** | likely separate (objective system) |

So filtering the WorldMapPointParam **category+grace rows** (via the show-predicate / areaNo-99) does
not touch fog, the player dot, player markers, or objectives — they aren't WorldMapPointParam rows.
(If you instead *clear `+0x398` wholesale*, fog/dot still survive, but player-markers/objectives are
unconfirmed — another reason to prefer the targeted predicate filter.)

## 4. Recommendation

1. **Primary — show-predicate filter (safe, reuses existing code).** Make every category+grace
   `WorldMapPointParam` row the overlay draws fail the show-predicate (the mod's existing
   `areaNo→99` live-param edit, applied to all such rows). They never enter `+0x398`; nothing to
   clear; fog/dot/markers/objectives untouched. Then drop the `discover_flag` dedup in
   `map_renderer.cpp` and draw ALL graces.
2. **Fallback — clear `+0x398`.** Only if a row can't be predicate-filtered. It's a `std::map`;
   **don't free nodes by hand** (allocator-owned). Call the game's own `std::map::clear`/the
   manager reset — which requires pinning the real build/destructor first (the undefined fn at
   ~`0x140a82c30`; a one-shot `createFunction`+decompile in the Ghidra GUI, or a runtime "find what
   writes `[mgr+0x398]`" gets it). Clearing post-build also needs the render walk/reconcile to not
   re-insert that same frame — verify.

## 5. Runtime check to finish #1/#4 (cheap, decisive)
On the open map: read `mgr = [er_base+0x3D6E9B0]`, walk `[mgr+0x398]` (RB-tree), and for each node
value (`+0x28`) read its id (`+0x20`) / type. Count entries and compare to on-screen icons; confirm
graces+categories are present and whether any player-marker/objective ids appear there (vs absent →
separate manager). That settles which keep-items share `+0x398`.

## 6. AOBs / offsets
- `CSWorldMapPointMan` = `[er_base+0x3D6E9B0]` (getter slot `0x3D6E9D8`); built-icon map `+0x398`
  (std::map; node key`+0x20`/value`+0x28`).
- `FUN_140a82a80` (discover-toggle, the mod's current hook) AOB = `WORLDMAP_POINT_CTOR`.
- show-predicate vt[1] `FUN_140a81450`; per-row flag predicate `FUN_140d58470`; reconcile
  `FUN_140a832a0`; `_DiscoverMapPoint FUN_140a84080`; `CSWorldMapPointIns` ctor `FUN_140a811e0`.
- real build ~`0x140a82c30` (undefined in DB — define at runtime/GUI to get the clear/destructor).

Scripts: `find_suppresspins.java`, `find_realbuild.java`, `find_build3.java`.
