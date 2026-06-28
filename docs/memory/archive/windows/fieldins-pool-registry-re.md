---
name: fieldins-pool-registry-re
description: "FieldIns/loot RE CLOSED: sealed-chest loot NOT pre-open resident (lotId only in ItemLotParam); spawned-item MapIns record has lotId+pos+MapId but is transient. Ship baked partName-join"
metadata: 
  node_type: memory
  type: project
---

**★★★ RECONCILED FINAL (2026-06-23, merge commit 084d422): sealed-chest loot is NOT pre-open resident →
premium SHELVED; ship baked `partName`-join.** Two findings reconciled in
`windows_fieldins_registry_layout_and_preopen_re_findings.md` §7–§9:
- **§8 (<user>'s controlled in-process `[LOTSCAN]` at the UNOPENED chest, commits ac63c8c..c148fe4) = the
  deciding evidence:** scanning all committed PRIVATE mem for lot `0x3dd6fec4` at the unopened chest found it
  ONLY in the always-loaded `ItemLotParam` tables (2 hits), ZERO game objects → no per-chest object pre-open.
- **§7 (my uncontrolled full-mem scan, `lot_pos_scan.py`/`lot_obj_dump.py`/`mapins_*.py`) found a real
  record** — node `{lotId@+0,flag@+4,FieldIns*@+8}`, `FieldIns→"アイテム"+lotId@+0x50`, **MapId@node−0xD8**
  (m60_37_50_00) + **local pos@node−0xD4** (56.32,238.13,52.68) → abs pos computable — **but only because the
  item was already SPAWNED** (post-open/live pickup). That record is transient; it does NOT revive the
  sealed-chest feature. Owner = `CS::MapIns` region (vt 0x2a8d6d8); chest pool @+0x240 empty (lot is an
  inline record field, not a pool entry).
- Net: sealed chests stay baked-only. The §7 spawned-item layout is documented for any future
  **live-dropped-loot** tagging, but is out of explore-cache scope. Investigation CLOSED.

**MAP-STREAMING explains it structurally (2026-06-24, commit ba3c4bb, doc `windows_map_streaming_structures_re_findings.md`).**
ER streams maps live: `CSMapbndResCap` 267 (disk .mapbnd, raw @+0xC0), `CSMsbPartsGeom` **8001** + `CSMsbPartsMap`
343 = parsed parts WITH positions (resident). BUT `CSMsbEvent` = **1** → MSB events incl. Treasure are parsed
then DISCARDED → the itemLotId is stored nowhere resident (0 hits scanning all MSB structs for the chest lot).
So positions are resident, the lot↔part link is not = the structural cause of §8. Only memory route to an unopened
lotId = parse the raw .mapbnd Treasure table from CSMapbndResCap+0xC0 = identical to the offline bake (redundant).
Keep baked DB as identity source. (Possible cleaner join: CSMsbPartsGeom carries MSB EntityID — untested, marginal.)

**★ REACH SOLVED (2026-06-23 live, commit 42cce19, doc `windows_mapins_to_record_reach_re_findings.md`).**
Deterministic loaded-loot walker (no singleton chain): **enumerate MapIns by vtable-scan `er+0x2a8d6d8`**
(343, patch-stable via rtti_index) → **node = MapIns+0x460** `{lotId@+0, flag@+4, FieldIns*@+8}` →
**VALIDATE `*(u32)(FieldIns+0x50)==lotId`** (gate = never emits garbage) → **MapId@m+0x388** (=node−0xD8),
**localPos@m+0x38c** (=node−0xD4) → absolute pos = gridXZ·256+local; identity via resolve_loot_item_textid.
Live-proven (mapins_reach_check.py) + cross-session stable (matches §7). ⚠️ Corrections: er+0x485cbb8 is
NOT WorldMapManImp (dropped); "record NOT in body" was a scan-range artifact (it's at +0x460). Scope
re-confirmed = loaded SPAWNED/OPENED loot only (1/343 item-bearing = the one opened chest; §8 holds for
sealed). Generalisation to ≥3 nodes blocked by game state (only 1 item resident); the self-validating gate
makes the walker safe anyway. Hedge: also scan MapIns body +0x100..+0x800 for the node signature, same gate.
Tools: `live_reach.py`, `mapins_reach_check.py`. ↓ enum-anchor detail (singleton chain, superseded by the vtable-scan reach):

**ENUMERATION ANCHOR (2026-06-23, commit 188d977, doc `windows_mapins_loot_record_enum_re_findings.md`).**
For enumerating loaded MapIns (where post-spawn loot records live). Chain (Ghidra via the new tools):
`WorldMapManImp` (FD4Singleton vt 0x2a8f918) `+0x28`=count, **`+0x5d0` WorldBlockMap[] stride 0x220**
(ctor FUN_1407309f0) → `WorldBlockMap` (vt 0x2a8f650) **`+0x110` CSGrowableNodePool<MapIns*>** (cap@+0x120,
node_arr@+0x128 stride8) → `MapIns` (vt 0x2a8d6d8). WorldMapManImp singleton slot **CANDIDATE er+0x485cbb8**
(unconfirmed). **PROVEN fallback (live): bounded vtable-scan for MapIns er+0x2a8d6d8 = 343 instances** —
ship-ready, no chain. Each MapIns embeds FieldInsBase pool @+0x240 (empty for chest); loot = inline record
found by `FieldIns+0x50==lotId` signature, MapId@node−0xD8, pos@node−0xD4. **LIVE VALIDATION PENDING**
(game closed): re-run `<ghidra_scripts>\live_mapins_anchor.py` to confirm the slot + ~343 yield + the
placed-world-item residency. FieldArea (0x3d691d8) was a red herring (sibling mgr, not the container).

**[superseded] §6 verdict (commit c7cc461): "link DEAD".** Live RPM
(`<ghidra_scripts>\registry_layout_check.py`/`registry_rtti_check.py`) proved the singleton `er+0x3d7b0c0`
is `CS::RendManImp` (render mgr, vt 0x2b9f148); its `[+0x10]→[+0x720]` container map@+0x18 (13 entries,
keyed idx|MapId) holds `CS::CSWorldGeomStaticIns` (vt 0x2a86860, pos@+0x250, **lotId@+0x50 = 0**) = the
same geom assets the geom walk already sees, NO lotId. lotId lives only on a spawned pickup (open-time) or
the baked ItemLotParam table. The chain/node-layout below were structurally CORRECT (verified live:
container=[sub+0x720], two std::maps, _Mysize @+0x10/+0x28, instance@node+0x30, callback@node+0x28); only
the *content* interpretation (render-side geom, not loot) changed. Trail below. [[runtime-icon-coverage]].

FieldIns (field-gimmick / loot) RE for the explore-cache "added-loot-with-names" supplement.
Full writeup: `docs/re/windows_fieldins_pool_anchor_and_join_re_findings.md` (Ghidra section dated
2026-06-23). Scripts `<ghidra_scripts>\find_fieldins{,2..6}.java`, raw out `out_fieldins{3..6}.txt`.
See [[ghidra-worldmap-re]] for the Ghidra headless setup, [[runtime-icon-coverage]] / [[rpm-live-memory-tooling]].

**Confirmed RTTI:** pool = `CS::CSGrowableNodePool<CS::FieldInsBase*>` vtable RVA `0x2a84ca0`;
`FieldInsBase` vtable `0x2a25e68`. Class chain `FieldInsBase ← CSWorldGeomIns/MapIns ← CSWorldGeomStaticIns…`.

**STEP 1 SOLVED — static anchor + iterable registry.** Each FieldIns/geom/MapIns instance
self-registers into an FD4Singleton-owned RB-tree (asserts cite `FD4Singleton.h`), keyed by a 64-bit
MSB-derived id. Anchor = **`er_base+0x3d7b0c0`** (per-frame field-step singleton; AOB `48 8B 05 ?? ?? ?? ??`
@0x6c5b78 disp 0x036B5541, or `48 83 3D ?? ?? ?? ?? 00` @0x72e5d6). Chain: `reg=[er+0x3d7b0c0]`,
`sub=[reg+0x10]`, `map=sub+0x720`, RB-tree header `@map+0x8`. Node: +0x20 key(u64), **+0x28 value =
instance ptr**, +0x00/+0x08/+0x10 = L/parent/R, +0x19 nil flag. Iterator `FUN_140b32d00` (per-frame),
add `FUN_140b32880`, rm `FUN_140b32b90`. ⇒ resident field-instance set IS iterable from a static base.

**RUNTIME RESULTS (in-process probes, 2026-06-23) + 2nd Ghidra pass (find_fieldins7/8, commit da19285):**
- **Path (A) embedded pool @ inst+0x3A8 = EMPTY at tile-load** on all 24 assets → loot child NOT parented
  to the asset until it spawns. Dead as a passive walk.
- **Path (B) registry walk — CORRECTED layout** (my first-session offsets were wrong; doc
  `windows_fieldins_registry_layout_and_preopen_re_findings.md`): `reg=[er+0x3d7b0c0]`, `sub=[reg+0x10]`,
  **`container=[sub+0x720]`** (extra DEREF I missed). Two std::maps in container; field instances
  self-register into the map @ **`container+0x18`** (head `*(container+0x20)`, **_Mysize `*(container+0x28)`**).
  Node = MSVC RB-tree: _Left+0x00/_Parent+0x08/_Right+0x10/_Isnil+0x19(byte)/key u64 +0x20/
  callback +0x28/**INSTANCE +0x30** (read +0x30, NOT +0x28 — +0x28 is the FUN_1406c6340 callback, why
  the probe saw bogus vt 0x30308e8). Per-frame `FUN_140b32d00` iterates a DIFFERENT map @container+0x00.
- **Taxonomy:** FieldInsBase subclasses = ChrIns / CSBulletIns / HitInsBase / CSWorldGeomIns / MapIns.
  **NO item/treasure class** → the lotId "アイテム" object is a geom-item (placed/dropped), not distinct.
- **★ Make-or-break residency = leans OPEN/SPAWN-time for sealed chests** (path-A empty + no item class +
  a FieldInsBase has vtable not a name @+0x00, so the old "name@+0x00+lotId@+0x50" scan hit = ItemLotParam
  copy or already-spawned pickup, not a pre-open FieldIns). Premium realistically limited to placed/dropped
  world loot; sealed-chest contents stay baked-only. **NEXT runtime test (unblocked):** walk container+0x18
  reading instance@node+0x30 + lotId@+0x50, at unopened chest `AEG099_090_9000`/`0x3dd6fec4` then after open.
