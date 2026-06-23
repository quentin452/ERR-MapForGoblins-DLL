# Findings — FieldIns registry exact layout + pre-open residency (Ghidra)

Answers `windows_fieldins_registry_layout_and_preopen_re_prompt.md` (commit 160a01a). Ghidra pass
2026-06-23, scripts `D:\ghidra_scripts\find_fieldins7.java` / `find_fieldins8.java`, raw out
`out_fieldins7.txt` / `out_fieldins8.txt`. Builds on the STEP-1 anchor (commit be1b018).

---

## 1. ★ CORRECTED registry chain + node layout (path-B's 2-node walk was TWO wrong offsets)

The in-process path-B walk reached ~2 nodes because it (a) skipped a deref and (b) read the wrong
value word. Decompiling the add `FUN_140b32880`, insert `FUN_140b32010`, node-init `FUN_140b31780`
and the per-frame iterator `FUN_140b32d00` gives the exact layout:

```
singleton = er_base + 0x3d7b0c0                 (FD4Singleton; AOBs in the pool-anchor findings)
reg       = *(u64*)singleton
sub       = *(u64*)(reg + 0x10)
container = *(u64*)(sub + 0x720)                 ← MISSING DEREF: it is *(sub+0x720), not sub+0x720
```

`container` holds **two** allocator-aware `std::map`s (layout each: `allocator* @+0x00, _Myhead @+0x08,
_Mysize @+0x10`):
- **map@`container+0x00`** — the one the per-frame `FUN_140b32d00(container,0)` iterates
  (`head = *(container+0x08)`). An "active/update" subset.
- **map@`container+0x18`** — **where field instances self-register** (`FUN_140b32880` inserts at
  `param_1+0x18`; head = `*(container+0x20)`, count `_Mysize = *(container+0x28)`). **Use this one to
  enumerate registered instances.**

**RB-tree node layout** (MSVC `std::map`, key=u64, 24-byte value):
```
node+0x00  _Left
node+0x08  _Parent
node+0x10  _Right
node+0x18  _Color (byte)
node+0x19  _Isnil (byte)   ← sentinel/leaf guard; stop here
node+0x20  key   (u64)     = CONCAT44(short@instance+0x2c , MSB-derived u32)
node+0x28  value word A    = a per-instance CALLBACK fn ptr (FUN_1406c6340)   ← NOT the instance
node+0x30  value word B    = the INSTANCE pointer (this)                      ← read THIS
```
- begin = `_Myhead->_Left = *(_Myhead+0x00)`; in-order successor until back to `_Myhead`; skip `_Isnil`.
- **Bulk-read:** size the walk with `_Mysize @ container+0x28` (don't blind tree-walk under Wine).
- The probe read `node+0x28` (the callback) as the instance → its "vtable" `0x30308e8` was a code ptr,
  not an object. **Read `node+0x30`.** And it walked `sub+0x720` as the map (no deref, head at +0x8) →
  that's the *other* (active) map, and without the `[sub+0x720]` deref it was reading garbage.

## 2. What the registry contains — FieldInsBase taxonomy (so the filter is correct)
`FieldInsBase` (vtable RVA `0x2a25e68`) subclasses, from their ctors (refs to that vtable):
| ctor | class |
|---|---|
| `FUN_1403e7940`, `FUN_1403e6e00` | `CS::ChrIns` (characters/enemies — lotType-2 enemy drops live here) |
| `FUN_140395b00`, `FUN_140395810` | `CS::CSBulletIns` (projectiles) |
| `FUN_1407089f0`, `FUN_1407089c0`, `FUN_140708980` | `CS::HitInsBase` (hit volumes) |
| `FUN_1406c5900` | `CS::CSWorldGeomIns` (placed assets) → `CSWorldGeomStaticIns` (`FUN_1406db840`) / `CSWorldGeomDynamicIns` (`FUN_1406b9880`) |
| `FUN_14071a0f0`, `FUN_140719f10` | `CS::MapIns` |

So the registry = **every loaded field instance** (chr/bullet/hit/geom/map), keyed by id. **There is NO
dedicated "item/treasure" FieldIns subclass.** The lotId-bearing "アイテム" object the prior heap-scan
found is therefore a **geom** (placed/dropped item = `CSWorldGeom*Ins`), not a distinct class.

## 3. ★ Make-or-break: pre-open residency — strong lean to OPEN/SPAWN-time for sealed chests
Static evidence converges:
- **Path A (runtime):** the chest asset's embedded `CSGrowableNodePool<FieldInsBase*>` @ `+0x3A8` is
  EMPTY at tile-load → the loot child is not parented to the asset until it spawns.
- **Taxonomy (this pass):** no item class; the lotId carrier is a geom-item, and a sealed chest's
  contents are not a world geom instance until the chest opens and spawns the pickup.
- **The earlier "FieldIns with name@+0x00 + lotId@+0x50":** a `FieldInsBase` has its **vtable** at
  `+0x00`, not an inline name — so that scanned object did **not** match the FieldInsBase layout. It was
  likely the `ItemLotParam`-table copy (the second family the scan itself noted) or an already-spawned
  pickup, **not** a pre-open resident treasure FieldIns.

**Assessment:** for **sealed vanilla chests**, the lotId-bearing instance is **not resident before the
chest is opened** → the explore-cache cannot reveal sealed-chest contents pre-open (and those are baked
anyway). For **free-standing world loot already placed as a geom-item (incl. ERR-added items dropped in
the world)**, the instance IS resident at tile-load and IS reachable via the corrected `container+0x18`
registry walk, filtered to geom-item instances. So the premium survives for placed/dropped loot, not
sealed chests.

## 4. Decisive runtime test (now unblocked by §1)
With the corrected chain, one in-process pass settles it:
1. Walk `container = [[er+0x3d7b0c0]+0x10]+0x720]`, map @ `container+0x18` (head `*(container+0x20)`,
   size `*(container+0x28)`); for each node read instance `= node+0x30`, its `vtable` and candidate
   `lotId@+0x50`. Log count + how many geom-class instances carry a real lotId.
2. Stand at the **unopened** known chest `AEG099_090_9000` (lot `1037500100`/`0x3dd6fec4`): is a
   geom-item instance with that lotId present? Then **open it** and re-walk. If it appears only after
   open → sealed-chest loot is open-time (confirm §3); if present before → premium is fully alive.

## 5. Net / decision
- **Path B is now correctly wired** (chain + node layout + `node+0x30` instance + `_Mysize`). The mod's
  `diag_fieldins_join` should walk `container+0x18` with these offsets.
- **Sealed-chest pre-open lotId = almost certainly NOT resident** → keep baked-only for chests; the
  "ERR-added-loot-with-names" premium is realistically limited to **placed/dropped world loot**, which
  the corrected registry walk can surface. Confirm with the §4 test before investing further.

Scripts: `D:\ghidra_scripts\find_fieldins7.java`, `find_fieldins8.java` (out `out_fieldins{7,8}.txt`).

---

## 6. ★★ LIVE RPM VERIFICATION (2026-06-23) — chain confirmed, but it's the RENDER geom list, NOT a loot registry
`D:\ghidra_scripts\registry_layout_check.py` + `registry_rtti_check.py`, live on pid running ERR.

**The chain + node layout from §1 are STRUCTURALLY CORRECT** (resolved live, no guessing):
`slot=base+0x3d7b0c0 → singleton=[slot] → sub=[singleton+0x10] → container=[sub+0x720]`. Container holds
the two `std::map`s exactly as documented: map@+0x00 (`_Myhead=[+0x08]`, `_Mysize=[+0x10]`=16) and
map@+0x18 (`_Myhead=[+0x20]`, `_Mysize=[+0x28]`=13). Node fields confirmed: key u64@+0x20,
**callback@+0x28 = `FUN_1406c6340`** (a code ptr — exactly why the earlier probe's "vtable 0x30308e8"
was bogus), **instance@+0x30**.

**But RTTI of the live objects rewrites the interpretation:**
- **The singleton `0x3d7b0c0` is `CS::RendManImp`** (render manager), vt RVA `0x2b9f148` — NOT a
  field/gimmick step singleton. The earlier "FD4Singleton field-instance step" label was wrong.
- **map@+0x18 = 13 × `CS::CSWorldGeomStaticIns`** (instance vt RVA `0x2a86860`), keyed
  `(index 1..13) | 0x3c000099`. Each carries a **position vec @+0x250** (matches the geom walk) and
  **`lotId@+0x50 = 0`**. So this is **RendManImp's list of loaded static-geom assets** — the SAME
  objects `goblin_collected.cpp` already walks via `CSWorldGeomMan`, with **no lotId**.
- map@+0x00 = 16 entries keyed by MapId (`0x3c0X0Y02`) → per-map-tile render objects. Also render-side.

**VERDICT — path (B) REFUTED LIVE.** `[[er+0x3d7b0c0]+0x10]+0x720]` is RendManImp's render-side
loaded-geom registry, not a loot/FieldIns registry; it holds `CSWorldGeomStaticIns` with positions but
**zero lotId**. Combined with path (A) (embedded pool empty at load), **the runtime asset→lotId link is
NOT reachable from any structure found** (asset instance, embedded pool, or this registry). The lotId
lives only on a **spawned pickup** (open/spawn-time) or in the **baked `ItemLotParam` table**. The
prior heap-scan hit (`lotId 0x3dd6fec4` resident, "name@+0x00") was therefore the `ItemLotParam`-table
copy / an already-spawned pickup, **not** a pre-open per-chest FieldIns — there isn't one.

**Net decision (final): the "ERR-added-loot-with-names from sealed structures" premium is DEAD** — keep
the baked `partName`-join (works for all baked loot today). Free-standing/dropped world pickups, if any,
remain catchable only as live spawned geom-items (not pursued). Stop chasing the runtime asset→lot link;
RPM + Ghidra have exhausted it. Scripts: `registry_layout_check.py`, `registry_rtti_check.py`.

---

## 7. ★★★ REVERSAL — live full-mem scan FINDS lotId + position + MapId co-resident (2026-06-23)
§6's "dead" verdict was premature: it only checked the RendManImp registry + the asset-embedded pool. A
**full live-memory scan for the lotId** (`D:\ghidra_scripts\lot_pos_scan.py` → `lot_obj_dump.py` →
`mapins_enum.py` / `mapins_verify.py` / `mapins_final.py`) found the real resident link.

**The resident loot node (chest `AEG099_090_9000`, lot `1037500100`/`0x3dd6fec4`):**
- A 16-byte node `{ lotId u32 @+0x00, flag u32 @+0x04 (=1), FieldIns* @+0x08 }` at a heap addr (live
  `0x22d12efeb90`). The `FieldIns*` → object with inline wide name "アイテム…" @+0x00 and **`lotId@+0x50`
  again** (the self-validating signature: `*(u32)(FieldIns+0x50) == node.lotId`).
- **In the SAME heap object, at fixed negative offsets from the node:**
  - `node − 0xD8` = **MapId `0x3c253200` = `m60_37_50_00`** (area 60, gridX 37, gridZ 50).
  - `node − 0xD4 / −0xD0 / −0xCC` = **local position `(56.32, 238.13, 52.68)`** (matches the baked
    chest pos). Between pos and the node sits a regular `{ffffffff,00000000}` slot array → the negative
    offset is a structural record header, not coincidence.
- **→ absolute world position is computable on the spot:** `worldX = gridX·256 + localX = 37·256+56.32 =
  9528.3`, `worldZ = gridZ·256+localZ = 50·256+52.68 = 12852.7`, Y `= 238.1`. (Matches the player→map-UI
  transform `world = gridXZ·256 + local`, [[ghidra-worldmap-re]].)

**Owner = a `CS::MapIns` region** (scan-back RTTI: `MapIns` 0x2a8d6d8, `CSMsbPartsMap` 0x2ba68f0,
`MapRes` 0x2a8db20, `CSGrowableNodePool<FieldInsBase*>` 0x2a84ca0, repeating ~0x2b0 stride). 343 MapIns
resident. NB: the chest's pool @MapIns+0x240 is EMPTY (`node_arr=0`) — the lot node is an INLINE record
field, not a pool entry (so path A's empty-pool result was a red herring, not proof of absence). The
literal offsets (`+0x460` node, `+0x384` MapId, `+0x38c` pos) are object/record-specific, not a uniform
MapIns field — only the loaded item-bearing record exposes them, which is why only 1/343 matched a fixed
offset.

**Status: the link is ALIVE for LOADED loot.** A resident record carries lotId + name + local pos +
MapId together → full item identity AND absolute position, for loot currently streamed in (the
explore-cache scope). Residency caveat from §3 still applies (sealed-chest contents may be open-time;
this scan's state was not controlled — re-test at a known-unopened chest). **Remaining work = a stable
enumeration anchor:** find all loaded loot nodes from a static base (walk the MapIns set, or the owner of
the records) rather than a full-mem lotId scan. Signature for the mod/cache: a node `{L, flag, P}` with
`*(u32)(P+0x50)==L`; then `MapId = *(u32)(node−0xD8)`, `localPos = *(vec3)(node−0xD4)`.

Scripts: `lot_pos_scan.py`, `lot_obj_dump.py`, `mapins_enum.py`, `mapins_verify.py`, `mapins_final.py`.
