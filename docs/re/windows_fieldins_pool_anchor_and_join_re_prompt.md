# RE prompt — FieldIns pool static-base + geom→FieldIns join key (wire the explore-cache)

> **Goal: make the runtime asset→ItemLotID link USABLE.** The link itself is already PROVEN to exist
> (`windows_runtime_asset_to_itemlot_re_findings.md`): a loaded treasure's `itemLotId` rides a runtime
> `FieldIns` (field-gimmick) instance, pooled in `CS::CSGrowableNodePool<FieldInsBase*>`, with
> `lotId @ FieldIns+0x50` + inline wide name @+0x00. What's MISSING is the two things needed to walk it
> from the mod: **(1)** a stable STATIC BASE to that pool, and **(2)** the JOIN KEY that takes a geom
> chest we already walk → its FieldIns → its lotId. Deliver both. App = current ERR build; re-anchor
> every RVA/AOB.

---

## 0. Scope — read first (unchanged from the prior RE)

This is a SUPPLEMENT, not a bake replacement. FieldIns are LOADED-only instances (same physics as
`CSWorldGeomStaticIns`, proven `e1b502b`) → this upgrades runtime identity for LOADED tiles only,
feeding an explore-cache / added-loot coverage layer. Baked map stays mandatory + complete. Don't
re-chase a global registry — it doesn't exist. This is a quality / future-proofing play (catch ERR's
additive loot, with names, as the player walks in).

**Everything around the link is already RE'd + running — reuse, don't rebuild:**
- Geom walk (loaded chest pos + part name): `CSWorldGeomMan` → BlockData rb-tree @+0x18 → geom_ins vec
  @BlockData+0x288 → `CSWorldGeomStaticIns` @geom_ins+0x48, name@+0x00, pos vec4 @+0x250. In
  `src/goblin_collected.cpp`.
- `lotId → item identity`: `resolve_loot_item_textid` reads `ItemLotParam` slot-1 (goblin_inject.cpp).
- The FieldIns record: node `{lotId(u32)+flag, FieldIns*}` in `CSGrowableNodePool<FieldInsBase*>`;
  `FieldIns+0x50 = lotId`, `FieldIns+0x00 = inline wide name`. Found resident in the loaded-object
  heap (region `0x22d3c6e…` in the probe session). Tools: `D:\ghidra_scripts\asset_lot_probe.py`,
  `lot_resident_search.py`, `treasure_record_probe.py`.

---

## STEP 1 — static base to the FieldIns pool / manager

We can find the pool by heap-scanning for a known lotId today, but that's not anchorable. Need a
`er_base + RVA` (or AOB → RVA) to reach it deterministically every launch.

- **Find the owner singleton.** RTTI-scan `eldenring.exe` cleartext for the manager that owns the
  `CSGrowableNodePool<FieldInsBase*>`: candidates `CSFieldInsMan`, `*FieldIns*`, `*Gimmick*`,
  `CSEventFlagMan` neighbours, or an `FD4Singleton`/`FD4ComManagerStep` that holds the pool. The pool
  RTTI is already confirmed `CS::CSGrowableNodePool<FieldInsBase*>` (scan-back 0x220 from the record).
- **From pool → singleton → static slot.** Once the owning singleton is identified, find its static
  load site: the `MOV RxX,[rip+disp]` that loads the singleton ptr (same pattern as the existing
  `ICON_MGR_SLOT_RVA = 0x3d6e9b0` / `WORLD_GEOM_MAN` anchors). Pin the AOB at that load site, capture
  the displacement → the `er_base + RVA` slot.
- **Walk from the static base to the pool's node array.** Document the chain singleton → pool object →
  node storage (array base + count/`_Mysize` + stride). `CSGrowableNodePool` grows — record the live
  count field and the node stride so the mod can iterate all resident FieldIns. (Compare to how
  `goblin_collected.cpp` walks the geom vec — same bulk-read-one-record discipline.)
- **Deliver:** AOB byte string (for `src/re_signatures.hpp`, AOB-pinned not hardcoded-RVA — see the
  file header) + the displacement RVA + the singleton→pool→nodes offset chain.

---

## STEP 2 — the join key: walked geom chest → its FieldIns

The mod walks `CSWorldGeomStaticIns` (has pos + part name). It needs to land on the SAME treasure's
`FieldIns` to read its lotId. Find the shared key.

- **Primary hypothesis = EntityID.** MSB `Parts.Asset` carries an EntityID; the spawned FieldIns
  should carry the same. Find the EntityID field on BOTH sides:
  - On `CSWorldGeomStaticIns` / its `MsbPart` @+0x48 — dump bytes for a known chest, find its MSB
    EntityID (known offline from the MSB).
  - On `FieldIns` — layout so far: name@+0x00, lotId@+0x50. Find its EntityID field (scan the same
    known chest's FieldIns bytes for the EntityID value).
  - **Confirm they match** for ≥3 distinct chests across ≥2 maps → EntityID is the join.
- **Fallback keys if EntityID doesn't match or is 0:** (a) part-name string match (geom name@+0x00 ↔
  FieldIns name@+0x00 — but verify the FieldIns name is the part name, not the item name "アイテム");
  (b) a back-pointer FieldIns→MsbPart or MsbPart→FieldIns (check one level of pointers each way);
  (c) spatial nearest (last resort, ambiguous — avoid).
- **Direction that's cheapest to wire:** ideally iterate the FieldIns POOL directly (Step 1 gives the
  full resident set) and read `lotId@+0x50` + pos, skipping the geom join entirely IF FieldIns carries
  position. So also: **does FieldIns carry a world position?** (scan its bytes for the known chest's
  vec4 / a transform ptr). If yes → the pool is SELF-SUFFICIENT (lotId + name + pos per loaded
  treasure) and Step 2's join is moot — that's the best outcome, call it out explicitly.

---

## Decisive CE / Ghidra experiment

1. Same known chest as the prior session: `AEG099_090_9000` in `m60_37_50_00`, baked
   `itemLotId = 1037500100 (0x3dd6fec4)`, its FieldIns already located in the loaded heap.
2. Step 1: trace that FieldIns' pool back to a singleton with a static load site → AOB + RVA.
3. Step 2: dump the chest's `CSWorldGeomStaticIns`/`MsbPart` bytes AND its `FieldIns` bytes; line up a
   shared EntityID (or prove FieldIns self-carries pos). Repeat for 2–3 more chests on another map.

---

## Deliverable — `windows_fieldins_pool_anchor_and_join_re_findings.md`

- **Step 1:** AOB (re_signatures style) + displacement RVA to the FieldIns pool/manager singleton +
  the singleton→pool→node-array offset chain (array base, count field, stride). Confirm the pool is
  iterable + resident from tile-load.
- **Step 2:** the join — EntityID field offsets on both geom-instance and FieldIns + match proof
  (≥3 chests / ≥2 maps); OR the chosen fallback; OR **FieldIns is self-sufficient (carries pos)** →
  no join needed (preferred — state which).
- **Negative results count:** if no static anchor is stable (pool address not reachable from any
  singleton), or EntityID is 0/unreliable AND no fallback joins, say so definitively → the explore-
  cache stays heap-scan-only / deferred.
- `lotType 2` enemy drops still secondary (lot on enemy ChrIns / WorldChrMan) — note, don't block.

## Method notes
- Crash-safe `ReadProcessMemory`/`rpm<T>`, never `__try` ([[clang-cl-seh-noinline]]).
- Wine: bulk-read whole records, 1 RPM/record; pool walk MUST be bulk + throttled or it's a Wine
  freeze cliff ([[linux-rpm-walk-danger]]).
- AOBs are patch-resilient, RVAs patch-fragile — pin code signatures, capture the displacement
  (`src/re_signatures.hpp` header explains the convention).
- Wiring after this lands (no further RE): pool walk (or geom walk + join) → `lotId` →
  `resolve_loot_item_textid` → diff vs baked markers → explore-cache + "added loot" toast.
