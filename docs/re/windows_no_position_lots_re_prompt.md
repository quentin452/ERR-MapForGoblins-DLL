# RE PROMPT — "no positions in vanilla": recover world positions for the 748 EMEVD-granted lots

You are a reverse-engineering agent on **Windows** with the live Elden Ring (ERR mod) process,
RPM, and memory-scan tooling. This prompt hands you a fully-scoped, already-narrowed problem —
**build on the facts below; do not re-derive them.**

## Background (already SOLVED — context only)
The disk-route MSBE parser is done and verified (`docs/re/windows_resident_msbe_layout_re_findings.md`,
`src/worldmap/msbe_parser.{hpp,cpp}`). It reads `Events.Treasure(type==4) -> {itemLotId, partIndex}
-> PARTS[idx] -> position` from the mod's real `.msb.dcx`, and recovers a world position for every
**treasure** lot. Cross-validated against `data/items_database.json`: parser ≈ DB `source=="treasure"`
(4075 vs 3986; m10 exact 107==107). Loot needs only zlib (DFLT) — Oodle confirmed irrelevant to
treasure (all 256 KRAK maps are treasureless; all 4075 treasures are in the 708 DFLT maps).

## The problem
`data/items_database.json` has **748 entries with NO position** (`x==y==z==0`, `partName==""`,
`source==None`). Every other source has a position (enemy 25608 from the enemy's MSB part
`c0000_xxxx`; treasure 3986 from the chest/corpse part; emevd 711). The 748 carry a real
`map`, `itemLotId` (all map-table, ≥1e7), `eventFlag`, and `items[]`, but **no world point** — so
they cannot be placed on the map.

### Evidence already gathered (trust these)
- **The 748 lotIds are NOT in the MSB at all.** Byte-scan of the decompressed MSBs for the lotId as
  LE-u32: m11_00 → 0/6 found, m20_00 → 0/66, m12_01 → 1/2. So these are **EMEVD-granted**, not placed
  as MSB Treasure/Part — which is exactly why the bake (an MSB-part join) left them position-less.
- **Two sub-classes:**
  1. **ERR-custom container (≈318/748): map `m60_44_60_00`**, lotId range `1044600000..1044604591`,
     `eventFlag == itemLotId`, items = ERR-Reforged additions ("Artifact Piece", "Dread Essence").
     Almost certainly **locationless by design** (custom drop/grant/shop mechanic, no world placement).
     Likely correct to EXCLUDE from the map — but confirm.
  2. **Scattered vanilla-ish EMEVD pickups (≈430/748):** m11, m20, m12, m21, m31, … Mostly
     consumables/crafting; some armor SETS share ONE flag (Raging Wolf 4 pieces `11001985..988` all
     flag `11007985` = a single ground-pickup grant). ~262 follow `eventFlag == itemLotId + 7000`.
     A position MAY be recoverable for these.

### Concrete targets (start here)
```
m10_01_00_00 lot=10010100  flag=10017100     talisman  'Lunar Princess' Exultation'
m11_00_00_00 lot=11000195  flag=11007195     material  'Smithing Stone [6]'
m11_00_00_00 lot=11001985  flag=11007985     armour    'Raging Wolf Helm'   (set shares flag)
m11_10_00_00 lot=11100900  flag=11107900     armament  'Clinging Bone'
m12_01_00_00 lot=12010930  flag=12017930     armament  'Fire Longsword'
m60_44_60_00 lot=1044600000 flag=1044600000  material  'Artifact Piece'  (ERR-custom cluster)
```

## Your questions (in order)
1. **EMEVD → position (static route).** EMEVD files are on disk at `mod/event/*.emevd.dcx` (517 files,
   DCX_KRAK = Oodle; same wrapper as MSB; `common.emevd.dcx` + `common_func.emevd.dcx` hold shared
   templates). For a target flag/lot, find the instruction that awards the lot or sets the eventFlag
   (e.g. `AwardItemLot`, `HandleItemPickup`, the flag-set family). Does that instruction (or its
   enclosing event) reference an **entity ID / region ID / asset ID** whose position is then resolvable
   from the MSB POINT (region) or PARTS section? i.e. is there a recoverable `flag/lot -> entityId ->
   MSB position` chain? Document the exact instruction opcodes + arg layout if so.
2. **Runtime RPM (live route).** With the map loaded, is there a live object — item-glow `FieldIns`,
   map-region trigger, NPC/enemy, or `WorldMapPointParam` row — that carries this `itemLotId` or
   `eventFlag` AND a world position? Reuse the existing infra: the §8 FieldIns reach
   (`MapIns+0x460 {lotId, flag, FieldIns*}`, validator `*(FieldIns+0x50)==lotId`, see
   `loot-identity-stable-err-additive`) and the WorldMapPointParam / WorldChrMan enums. Can you read a
   position at runtime that the static files lack?
3. **WorldMapPointParam check.** The bake already reads `WORLD_MAP_POINT_PARAM_ST`. Do any of these 748
   eventFlags / lots correspond to an existing WorldMapPointParam row (which has a position)? If the
   game itself already knows a map point for these, that's the cheapest source.
4. **Classify the 318 ERR-custom (m60_44_60_00).** Confirm they are genuinely locationless grants
   (shop / custom-mechanic reward) with no meaningful world point → exclude from map. Or, if EMEVD ties
   them to a trigger location, report it.

## Deliverable
A findings doc (mirror the MSBE one) that, per sub-class, states:
- **Recoverable?** yes/no, and via which route (EMEVD-static / runtime-RPM / WorldMapPointParam).
- **The exact chain** to get a position: file/param + offsets, or instruction opcode + arg index +
  the entity/region lookup, or the RPM pointer path. Enough to write the C++ directly.
- **The genuinely-locationless set** (e.g. shop/custom grants) to exclude, with the rule that
  identifies them (e.g. `lotId in 1044600000.. && map==m60_44_60_00`, or `eventFlag==lotId`).

Net goal: turn as many of the 748 position-less lots into placeable map markers as the data allows,
and cleanly identify the residue that has no world position by design.
