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
