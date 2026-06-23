# RE prompt — runtime LOADED-asset → ItemLotID link (for the explore-cache / coverage supplement)

> **Goal: find the runtime structure that maps a LOADED treasure/asset instance to its ItemLotID**,
> so the overlay can identify what a loaded chest/corpse contains at runtime WITHOUT the baked
> `items_database` join. This powers a runtime **coverage supplement** — catching ERR's added loot
> (additive mod) with full identity the moment the player walks into a tile, and feeding a
> progressive-reveal explore-cache. App = current ERR build; re-anchor every RVA/AOB.

---

## 0. Scope honesty — read first (this is a SUPPLEMENT, not a bake replacement)

A prior analysis settled that a live-only map CANNOT replace the bake:
- `e1b502b` (`windows_global_item_position_structure_re_findings.md`) **proved** per-tile MSB
  streaming, **no global registry** → unvisited tiles aren't in RAM. Loaded-only is a hard physics
  limit, not a missing structure. Don't re-chase it.
- Reconstruction census of the 8653 baked markers: **34% asset-backed, 49% lot-backed (identity
  needs the asset→lot join — THIS RE), 16% geom-blind (EMEVD/MSB-regions, never in the geom walk).**

So **baked stays mandatory + complete**. This RE only upgrades what the runtime can identify FOR
LOADED TILES → it makes the explore-cache/coverage layer high-quality (catch added loot, with names),
it does NOT enable removing the bake. Weigh effort accordingly; it's a quality/future-proofing play.

---

## 1. The missing link (everything around it is already solved — reuse, don't rebuild)

- **Loaded asset position + part name** = ALREADY walked: `CSWorldGeomMan` → BlockData rb-tree @+0x18
  → geom_ins vec @BlockData+0x288 → `CSWorldGeomStaticIns` @geom_ins+0x48 (RTTI per `e1b502b`),
  name@+0x00, runtime pos vec4 @+0x250 (or MsbPart+0x20). Running in `src/goblin_collected.cpp`.
- **lotId → item identity** = ALREADY live: `resolve_loot_item_textid` reads `ItemLotParam` slot-1
  (goblin_inject.cpp). Give it a lotId, it yields the item textId/icon.
- **MISSING = the join in the middle**: loaded treasure/asset instance → **its `ItemLotID`**. Offline
  this is the MSB `Events.Treasure` (chest part → itemLotId), baked into `data/items_database.json`
  as `(map, partName) → itemLotId`. **We need the RUNTIME equivalent so no baked join is required.**

---

## 2. Find it

ER parses MSB `Events.Treasure` at map load into runtime state (it must, to spawn the item when the
chest opens). Locate where the loaded-treasure→lot association lives.

- **On the asset instance itself**: inspect `CSWorldGeomStaticIns` (and its `MsbPart` @+0x48) for an
  `itemLotId` / treasure-record field or a pointer to its Event.Treasure record. Dump the instance
  bytes for a KNOWN chest and look for its known itemLotId value (from `items_database.json`).
- **A treasure/item-lot manager**: RTTI scan `eldenring.exe` cleartext for `*Treasure*`, `*ItemLot*`,
  `*Pickup*`, `*Gimmick*`, `CSEventFlagMan` neighbours, or an FD4Singleton that indexes loaded
  treasures by entity/part. Check whether it's keyed by the asset's EntityID (so we can join from the
  geom instance's entity/name).
- **The pickup/open path**: find the function that, on opening a chest, resolves its lot
  (`getItemLotId`-ish), then walk back to the resident structure it reads (the lot is known before the
  open — it's set at load). "Find what accesses" the known itemLotId in CE while standing at the chest.

---

## 3. Decisive CE experiment

1. Pick a loaded chest with a KNOWN itemLotId (from `items_database.json`, e.g. a Limgrave map chest).
2. Resolve its loaded `CSWorldGeomStaticIns` (geom-walk chain above) → note its address + EntityID.
3. CE: search memory for the known `itemLotId` (u32) near the instance, or "find what accesses" the
   instance when the tile loads / chest opens.
   - **Found resident** (field on the instance, or a manager record keyed by entity) → trace static
     base + pointer chain → §4 layout. The link EXISTS.
   - **Only appears transiently at open-time** (spawned, not stored) → the resident link may be absent;
     note that → we keep the baked join. (Plausible outcome — be ready to refute cleanly.)

---

## 4. Deliverable — `windows_runtime_asset_to_itemlot_re_findings.md`

- **EXISTS**: how to read a loaded treasure's `itemLotId` at runtime — offset on the geom instance
  OR a manager + key (EntityID/partName) + walk. Static base (AOB / `er_base+RVA`, add to
  `src/re_signatures.hpp` style). Per-entry layout. Confirm it's resident from tile-load (not only at
  open). Then: marker = geom pos (have it) + this lotId → `resolve_loot_item_textid` (have it) = full
  runtime loot marker, no baked `items_database`.
- **DOES NOT EXIST / open-time-only**: state it definitively → baked join stays. Still a useful result.
- **lotType 2 (enemy drops)**: secondary — that lot lives on the enemy ChrIns (WorldChrMan, loaded
  chars; the boss/enemy-pos enum was abandoned). Note feasibility, don't block on it.

## 5. Method notes
- Reuse: the geom walk + ItemLotParam live read + block→world transforms are all RE'd and running.
- Crash-safe `ReadProcessMemory`/`rpm<T>`, never `__try` ([[clang-cl-seh-noinline]]). Wine: bulk-read
  whole records, 1 RPM/record ([[linux-rpm-walk-danger]]).
- Result feeds a SUPPLEMENT (explore-cache / added-content coverage) on top of baked — not a removal.
