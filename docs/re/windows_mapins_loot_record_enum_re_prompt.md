# RE prompt — MapIns loot-record enumeration anchor (wire the loaded-loot explore-cache)

> The resident loot record EXISTS (proven: `windows_fieldins_registry_layout_and_preopen_re_findings.md`
> §7, commit da513922): a node `{ lotId u32 @+0, flag @+4, FieldIns* @+8 }` whose `FieldIns` carries an
> inline "アイテム" name @+0 and **`lotId@+0x50`** (self-validating: `*(u32)(FieldIns+0x50)==node.lotId`),
> and the SAME object holds **MapId @ node−0xD8** + **local pos @ node−0xD4** → full item identity +
> absolute world position, for loaded loot. **What's missing = a STABLE way to ENUMERATE these records
> from a static base** so the mod can list all loaded loot deterministically. A full-memory byte scan
> CANNOT do it (§8: too slow under Wine; "アイテム" too common; the node↔FieldIns self-validation needs a
> structured walk). Deliver the enumeration anchor. App = current ERR build; re-anchor every RVA/AOB.

See `windows_fieldins_registry_layout_and_preopen_re_findings.md` §7 (the record) + §8 (why brute scan
failed) + the prior chain/anchor work (be1b018, da513922).

---

## What we know (don't re-derive)
- **The record's owner is a `CS::MapIns` region** (da513922 RTTI scan-back: `MapIns` vt RVA `0x2a8d6d8`,
  neighbours `CSMsbPartsMap` `0x2ba68f0`, `MapRes` `0x2a8db20`, `CSGrowableNodePool<FieldInsBase*>`
  `0x2a84ca0`; repeating ~`0x2b0` stride). **343 MapIns resident** in that session.
- The chest's MapIns pool `@MapIns+0x240` was **EMPTY** (`node_arr=0`) → the lot record is an **INLINE
  record field on the MapIns (or a child it owns), not a pool entry** (so path A's empty embedded pool
  was a red herring).
- The literal offsets seen for ONE chest (`+0x460` node, `+0x384` MapId, `+0x38c` pos) were
  **record-specific** — only 1/343 MapIns matched at a fixed offset → the loot record is NOT a uniform
  MapIns field; it's reached via a sub-structure/pool/list the MapIns owns, OR only item-bearing MapIns
  expose it. Resolve the real containment.
- ⚠️ Unresolved contradiction to settle: §3 says `FieldInsBase` has its **vtable @+0x00**, but §7's
  record `FieldIns*` has an **inline name @+0x00**. So §7's pointed-to object is **not** a vtable'd
  `FieldInsBase` — identify what it actually is (an MSB `Event.Treasure` runtime record? a pickup
  gimmick? a `CSMsbPartsMap` entry?). Its class determines the right enumeration.

## STEP 1 — static base to the MapIns set
Find the manager that owns the 343 resident `CS::MapIns` and its static load site.
- RTTI/Ghidra: locate the `MapIns` container (the world/map manager — `CSMsbPartsMap` / `WorldBlockMap`
  / `CS::MapInsManager`-like, or a list hanging off `WorldChrMan`/`CSFD4FileMan`/the map-tile streamer).
  The `MapIns` ctor (`FUN_14071a0f0` / `FUN_140719f10`, per da51392 taxonomy) registers each instance —
  walk back to where the set/array/map is stored.
- Deliver the static slot (`er_base+RVA`, AOB-pinned per `src/re_signatures.hpp`) + the chain to iterate
  all loaded MapIns (array base + count/stride, or tree head + node layout). Confirm it yields ~343.

## STEP 2 — from a MapIns to its loot record(s)
For an item-bearing MapIns, find the path to the loot record `{lotId, flag, FieldIns*}` (and the
inline-name object).
- Decompile the MapIns ctor + the `Event.Treasure` / item-spawn parse to see where it stores the lot
  record (a child pool? an embedded list? a `CSMsbPartsMap` of parts, one per placed item?). The
  record-specific offsets (`+0x460`/`+0x384`/`+0x38c`) are clues — express them relative to the
  containing sub-structure, not the MapIns base.
- Confirm the per-record fields generalise across ≥3 distinct loaded items / ≥2 maps:
  `lotId`, `flag`, `FieldIns*` (→ `name@+0`, `lotId@+0x50`), `MapId` (record−0xD8), `localPos`
  (record−0xD4, vec3). Give offsets relative to a stable record base, with the self-validation
  (`*(u32)(FieldIns+0x50)==lotId`) as the integrity check.

## STEP 3 — the make-or-break, now controllable
With enumeration in hand, settle residency cleanly:
- At a **known-UNOPENED** sealed chest (loaded): is its loot record present in the MapIns set? If NO →
  sealed-chest contents are open/spawn-time (keep baked-only for chests). 
- For a **free-standing world item** (glowing ground pickup, loaded, not taken): is ITS record present?
  Expect YES → the premium is alive for placed/dropped world loot (the ERR-added-loot scope).
- State which loot classes are enumerable pre-pickup.

## Deliverable — `windows_mapins_loot_record_enum_re_findings.md`
1. Static base + chain to iterate all loaded `MapIns` (AOB + RVA + array/tree layout + count). 
2. MapIns → loot-record(s) containment (sub-structure path + per-record field offsets), generalised
   across ≥3 items / ≥2 maps, with the self-validation check.
3. The class identity of §7's inline-name object (resolves the §3/§7 contradiction).
4. Residency verdict per loot class (sealed chest vs free-standing world item).
5. If any step dead-ends (no stable anchor, or records open-time only), say so → premium stays baked-only.

## Then (mod side, no further RE)
Re-point the dormant `diag_lot_memscan` / a new walker in `goblin_collected.cpp`: iterate MapIns →
records → `(lotId → resolve_loot_item_textid)` + `(MapId, localPos)` → an explore-cache of loaded
ERR-added loot with names + absolute positions, layered on top of the baked map.

## Notes
- Reuse: the geom walk, `resolve_loot_item_textid`, block→world transforms are all RE'd + running.
- Crash-safe `rpm`/`ReadProcessMemory`, never `__try` ([[clang-cl-seh-noinline]]). Wine: bulk-read whole
  records, 1 RPM/record; size walks by the count field — never blind tree-walk ([[linux-rpm-walk-danger]]).
- Scripts so far: `lot_pos_scan.py`, `lot_obj_dump.py`, `mapins_enum.py`, `mapins_verify.py`,
  `mapins_final.py`, `registry_layout_check.py`, `registry_rtti_check.py`.
