# EMEVD position-less lots (#2) — RE findings (2026-06-24)

Goal (handoff #2): recover world positions for the **748 lots that
`items_database.json` carries with `x==y==z==0`, `partName==""`** (no MSB Treasure
placement), so they can become map markers without the committed bake. This doc
reports the honest, measured coverage — it is **much lower than the prompt's
optimistic "~430 recoverable"**.

## Method
- `tools/resolve_emevd_positions.py` — for each position-less lot, search the FULL
  DarkScript3-decompiled ERR EMEVD corpus (516 `*.emevd.dcx.js` incl. common/
  common_func) for the lotId literal, collect co-occurring integers that resolve to
  a placed MSB **Part (enemy/asset)** OR **Region (POINT)** with a position (live
  SoulsFormats index, Parts + Regions + EntityGroupIDs). Classes: `lot==entity`
  (the lotId is itself a placed entity), `resolved-coarg`, `lot-in-emevd-no-coarg`,
  `absent-from-emevd`.
- `tools/check_posless_in_msb.py` — intersect the position-less lots with ALL `_00`
  MSB Treasure itemLotIds (i.e. what the #1 disk parser now emits).

## Result (measured over ERR 2.2.9.6)
```
748 position-less   = 318 ERR-custom (m60_44_60)  +  430 non-custom
430 non-custom:
   22  ARE MSB Treasures now emitted by #1 (the bake's Treasure-match missed them; FREE)
   50  EMEVD-resolvable to a position  (47 lot==entity  +  3 resolved-coarg; 3 overlap #1)
   -----
   69  union recoverable (#1 ∪ #2)
  361  STILL position-less  (real items, but no MSB/EMEVD world anchor)
```

### The three classes of the 430
1. **22 — actually MSB Treasures (already won by #1).** The committed bake marked
   them position-less, but they *are* `Events.Treasure` placements (e.g. **12010930
   "Fire Longsword"** m12_01). The #1 disk parser emits them with the correct
   position at runtime → **no #2 work needed**, they're recovered for free.
2. **50 — EMEVD-anchored.** Split:
   - **47 `lot==entity`** — the lotId numerically equals a placed character/asset in
     the same map (boss/enemy drop; position = that entity). ⚠️ **Low confidence** —
     this is the exact byte-coincidence pattern the bake's `unreachable_only_lots`
     guard was built to suppress (a map-lot row whose id happens to equal an entity
     id can produce a *phantom* marker at the enemy's feet). Needs per-lot validation
     before trusting.
   - **3 `resolved-coarg`** — the lot literal co-occurs with a region/part entity in
     the same EMEVD line (e.g. **11000195** ← boss entity 11000499 via template
     90005300; **12040000** ← region 12042506). Higher confidence, tiny count.
3. **361 — genuinely position-less.** Real items (Raging Wolf armor set, Golden
   Runes, smithing stones, talismans like **10010100 "Lunar Princess' Exultation"**)
   but with **no recoverable world point**:
   - **149 share an eventFlag with another position-less row** = multi-item / set
     grants (one pickup delivers the whole set; the extra `ItemLotParam_map` rows are
     duplicates, not separate world placements — e.g. Raging Wolf 11001985-988 all on
     flag 11007985).
   - The rest are scripted grants (`AwardItemsIncludingClients(lot)` / `AwardItemLot`
     / common-event templates) fired by quest/area/kill events with no single world
     coordinate, or orphan `ItemLotParam_map` rows the engine never delivers.

### The 318 ERR-custom (m60_44_60)
`lotId 1044600000..`, `eventFlag == itemLotId`, ERR-Reforged additions ("Artifact
Piece", "Dread Essence"). **Locationless by design** (custom grant/shop mechanic, no
world placement) → correctly excluded from the map.

## Why the existing #2 tooling resolved 0/748
`tools/scan_emevd_awards.py` → `data/emevd_lot_mapping.json` (585 lots) +
`tools/enrich_fallback_with_emevd.py` map **enemy-drop** lots to enemy entities; none
of its 585 lots intersect the 748 position-less set. So the committed bake's posless
rows were never enriched. (It also only indexed Parts, never Regions.)

## Conclusion / recommendation
- **#1 already nets the only clean, free win here (22 lots).**
- **#2's incremental yield is ~47 markers** (EMEVD `lot==entity`/coarg not already in
  #1), and the bulk of those (47 `lot==entity`) are **low-confidence byte
  coincidences** that risk phantom markers.
- **361 + 318 = 679 of the 748 have no world position by nature** — they are rewards/
  grants/duplicates, not placements. No amount of RE puts them on the map honestly.
- A **runtime** EMEVD parser (517 KRAK/Oodle files, param tracing) is **not worth ~47
  markers**. If pursued at all, the right shape is a tiny **offline-generated
  `emevd_positions` table** for only the **high-confidence** subset (the 3 coarg +
  hand-validated boss drops), kept alongside the (already-baked) EMEVD/enemy slice.
- The migration plan already keeps the EMEVD + enemy slices baked, so **#2 is NOT a
  blocker for the no-bake treasure switch (#11)** — it's a marginal coverage
  improvement, not a prerequisite.

## Files
`tools/resolve_emevd_positions.py`, `tools/check_posless_in_msb.py` (probes);
`data/emevd_posless_resolved.json` (the 50 EMEVD-resolved, probe output). EMEVD JS:
`D:\tools\emevd_js\err` (see darkscript3-emevd-decompile memory).
