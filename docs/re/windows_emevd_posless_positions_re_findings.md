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

### ⚠️ PROOF that `lot==entity` is a coincidence (do NOT place these)
Traced the actual EMEVD template for a `lot==entity` case (11100900, an item that
also collides with placed enemy `c2500_9000` @ entity 11100900):
```js
$Event(90005774, function(X0_4, X4_4, X8_4) {       // common_func.emevd
    EndIf(EventFlag(X8_4));
    WaitFor(ElapsedSeconds(2) && EventFlag(X0_4));    // wait on a TRIGGER flag
    AwardItemsIncludingClients(X4_4);                 // award the LOT
});
// call: InitializeCommonEvent(0, 90005774, 11109656, 11100900, 11107900)
//   X0_4=11109656 trigger flag, X4_4=11100900 = the LOT, X8_4=11107900 guard flag
```
So `11100900` is the **lot** awarded when flag `11109656` is set — **not** a world
position. The enemy sharing id 11100900 is unrelated. Placing the marker at that
enemy would be a **phantom** (exactly what the bake's `unreachable_only_lots` guard
exists to prevent). **Conclusion: the 47 `lot==entity` lots are flag-triggered
grants, not placements — they are NOT recoverable and must not be force-placed.**
Recovering them would require multi-hop tracing of the trigger flag (X0_4) to
whatever sets it (region enter / boss kill / quest step) — usually no single point.

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
- **#1 already nets the only clean, free win here: 22 lots** (MSB Treasures the bake
  missed, now disk-emitted with correct positions — no extra work).
- **The 47 `lot==entity` are PROVEN coincidences (flag-triggered grants), not
  placements** — see the 90005774 trace above. Force-placing them = phantom markers.
  Net real EMEVD-recoverable beyond #1 = **3 reliable coarg lots** — not worth any
  pipeline/runtime machinery.
- **~725 of the 748 have no world position by nature** (361 flag-triggered grants /
  set-piece duplicates / orphan rows + 318 ERR-custom locationless + the 47 false
  `lot==entity`). No honest RE puts them on the map.
- A runtime EMEVD parser (517 KRAK/Oodle files + multi-hop trigger-flag tracing) is
  **not justified**. The existing offline enrich stage is correct to leave them
  position-less.
- **#2 is NOT a blocker for the no-bake treasure switch (#11)** — the migration keeps
  the EMEVD/enemy slices baked anyway. **Verdict: close #2 — the position-less lots
  are genuinely unplaceable; do not fabricate markers for them.**

## Files
`tools/resolve_emevd_positions.py`, `tools/check_posless_in_msb.py` (probes);
`data/emevd_posless_resolved.json` (the 50 EMEVD-resolved, probe output). EMEVD JS:
`D:\tools\emevd_js\err` (see darkscript3-emevd-decompile memory).
