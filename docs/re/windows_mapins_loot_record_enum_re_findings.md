# Findings — MapIns loot-record enumeration anchor

Answers `windows_mapins_loot_record_enum_re_prompt.md` (commit 2328294). Ghidra pass 2026-06-23 using the
reusable tooling (`tools/ghidra/rtti_index.txt` + `query.java`). Builds on the §7 record layout.

> ⚠️ **Live validation PENDING** — the game process closed before the live walk could run. The chain
> below is Ghidra-derived (static) + the per-instance offsets are from the §7 live dump. Re-run
> `D:\ghidra_scripts\live_mapins_anchor.py` (game open) to confirm the WorldMapManImp slot + that the
> chain yields ~343 MapIns. The **bounded vtable-scan fallback (STEP 0) is already live-proven**.

---

## STEP 0 — proven enumeration TODAY: bounded vtable-scan (no fragile chain)
Live this session: a bounded VirtualQuery walk of committed-PRIVATE memory for the `CS::MapIns` vtable
(`er+0x2a8d6d8`) returns **343 MapIns instances** (`mapins_enum.py`). This is the same pattern the mod
already ships (`scan_all_cursor_instances`) — works now, patch-robust (vtable from `rtti_index.txt`),
needs no manager chain. **Recommended enumeration** unless the static chain proves cheaper live.

## STEP 1 — the static chain (Ghidra-derived; the "clean" alternative)
`query.java` on the ctors found the container hierarchy:

```
WorldMapManImp (FD4Singleton, vtable er+0x2a8f918)
  +0x28  : int  count of loaded WorldBlockMap
  +0x5d0 : WorldBlockMap[]  (stride 0x220 ; memset 0x19800 = up to ~192 slots)   [ctor FUN_1407309f0]
     each WorldBlockMap (vtable er+0x2a8f650):
       +0x110 : CS::CSGrowableNodePool<CS::MapIns*>  (vtable er+0x2a8f640)        [ctor FUN_1407289c0]
          +0x120 : u32 cap
          +0x128 : MapIns*[]  node array (stride 8)
            -> CS::MapIns  (vtable er+0x2a8d6d8)                                   [ctor FUN_14071a0f0]
```
- `FUN_140727fb0` (WorldBlockMap ctor) is called only by `FUN_1407309f0` (a `WorldMapManImp` ctor) →
  the WorldBlockMap array is **embedded in WorldMapManImp at +0x5d0**, stride `0x220`, count @ `+0x28`.
- `FUN_1407289c0` (WorldBlockMap dtor) frees `this[0x22]` = the `CSGrowableNodePool<MapIns*>` whose
  vtable sits at WorldBlockMap **+0x110**; its node array is `this[0x25]` = **+0x128**, cap `+0x120`.
- **WorldMapManImp singleton slot — CANDIDATE `er+0x485cbb8`** (`DAT_14485cbb8`, the FD4Singleton-guarded
  global in the dtor `FUN_140730d40`). NOT yet confirmed as the instance pointer (the dtor's
  guard pattern is ambiguous) → **pin live**: read `[er+0x485cbb8]`, expect RTTI `WorldMapManImp`, then
  walk `+0x5d0` blocks. If wrong, the GetInstance (a `mov rax,[rip+slot]` + null-test reader of
  WorldMapMan) gives the real slot.
- NB: `FieldArea` (`er+0x3d691d8`) was an early red herring — it's a sibling field manager, NOT the
  MapIns container (the WBM ctor caller is WorldMapManImp, not FieldArea).

## STEP 2 — MapIns → loot record (per-instance)
Each `MapIns` embeds a `CSGrowableNodePool<FieldInsBase*>` at **+0x240** (ctor `FUN_14071a0f0`:
`this[0x48]=pool vtable`, node_arr `this[0x4b]`). For the chest this pool was EMPTY — the loot is an
**inline record** on the item-bearing MapIns, NOT a pool entry. Per-record fields (from §7 live dump,
relative to the lot node):
- node `{ lotId u32 @+0x00, flag @+0x04, FieldIns* @+0x08 }` ; `*(u32)(FieldIns+0x50)==lotId` (integrity).
- `MapId @ node−0xD8` (e.g. `0x3c253200` = m60_37_50_00) ; `localPos vec3 @ node−0xD4`.
- → absolute world pos = `gridXZ·256 + localPos`. Item identity via `lotId → resolve_loot_item_textid`.
- ⚠️ These were captured at ONE object in a POST-SPAWN state; the per-record offsets relative to the
  MapIns base are record-specific (§7: 1/343 at a fixed offset) → on a generalised walk, locate the
  record via the `FieldIns+0x50==lotId` signature inside the MapIns body, not a fixed MapIns offset.

## STEP 3 — residency (make-or-break) — already settled negative for sealed chests
§8 (controlled in-process scan at the UNOPENED chest) proved the lotId is resident only in the
`ItemLotParam` tables pre-open — **no per-chest loot record exists before the chest opens**. So the
enumeration surfaces loot only for ALREADY-SPAWNED / placed world items, not sealed chests. Confirm the
placed-world-item case live (a glowing ground pickup, not yet taken) once the game is up.

## Net
- **Enumeration anchor delivered:** STEP 0 vtable-scan (proven, ship-ready) + STEP 1 static chain
  (Ghidra-derived, slot candidate `er+0x485cbb8` to confirm live).
- **Class identity (resolves §3/§7):** the item-bearing object is a `CS::MapIns` (MSB map-part instance,
  RTTI-confirmed), with `CSMsbPartsMap` part data — NOT a vtable'd `FieldInsBase`; the "アイテム"
  inline-name object it points to is the loot gimmick payload.
- **Scope:** loaded, already-spawned/placed loot only (sealed chests stay baked-only, §8). The premium
  layer is viable for placed/dropped world loot; gate the mod walker on the `FieldIns+0x50==lotId`
  signature + a valid MapId.

Tooling: `tools/ghidra/{query.java,rtti_index.txt}`; live `D:\ghidra_scripts\live_mapins_anchor.py`
(pending re-run), `mapins_enum.py` (343 proven), §7 scripts.
