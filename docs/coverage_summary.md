# MapForGoblins vs MapGenie — coverage synthesis

Cross-checked the mod's marker counts (ERR profile) against MapGenie's full
category taxonomy for both maps. Per-type tables: [coverage_base.md](coverage_base.md)
(Lands Between) and [coverage_dlc.md](coverage_dlc.md) (Shadow Realm). Regenerate
with `tools/coverage_vs_mapgenie.py`.

**Baseline (resolved 2026-06-17):** the **vanilla** profile is now generated, so the
bug flag is clean — vanilla vs MapGenie is the same game. The numbers below are the
**vanilla** column unless noted; ERR is shown only where the overhaul's delta matters.
(MapGenie maps vanilla, so a vanilla negative is a *real* mod gap, not an ERR artifact.)

## The picture in three buckets

**1. Physically-placed content → covered well (✅ matches or exceeds MapGenie).**
Things with an MSB position. Close matches validate both the tooling and the
coverage:

| Type | mod / MapGenie (base) |
|---|---|
| Crystal Tear | 32 / 32 ✅ |
| Deathroot | 9 / 9 ✅ |
| Map Fragment | 19 / 19 ✅ |
| Bosses (all) | 164 / 170 (−6, near) ✅ |
| Invasion | 34 / 36 (−2, near) ✅ |
| Smithing Stones | 443 / 446 ✅ |
| Gathering materials | 1722 / 1009 (mod ≫, extensive nodes) |

(vanilla / MapGenie, base map.)

**2. Loot with no physical placement → systematic negative (confirmed design, not a bug).**
Equipment is the headline: nearly every type is 20–60% below MapGenie. Confirmed in
the **vanilla** column (same game as MapGenie), so this is a real extraction gap — and
ERR wires *more* than vanilla (e.g. weapons 246 > 188), which rules out "ERR removed it":

| Type | vanilla / MapGenie (base) | ERR |
|---|---|---|
| Weapon + Shield | 188 / 384 (−196) | 246 |
| Incantation | 34 / 101 (−67) | 51 |
| Ash of War | 62 / 105 (−43) | 64 |
| Talisman | 100 / 122 (−22) | 107 |
| Ammunition | 56 / 98 (−42) | 55 |

**Root cause (drilled).** The loot pipeline is a deliberately conservative
*collectible* tracker: it only emits a marker for one-time pickups that have both an
event flag and an MSB position. Four filters in `tools/generate_loot_massedit.py`
(lines 821–833) produce the shortfall, largest first:

- **`eventFlag > 0`** (l.824) — drops lots with `getItemFlagId = 0` (respawning /
  non-persistent drops, e.g. common enemy gear). Biggest contributor.
- **no-fallback** (l.822) — drops `ItemLotParam_map` rows never bound to an MSB
  Treasure event (exist in data, no physical placement).
- **shared enemy flag** (l.830–832) — collapses enemy drops that share one flag.
- **coord dedup** (l.532–545) — merges the `_00`/`_10` MSB platform duplicates.

Out of scope entirely: `ShopLineupParam` (merchant stock) and EMEVD-only awards with
no MSB entity. MapGenie shows *every* lot (incl. `eventFlag = 0` + fallback); the mod
shows only flagged, placed collectibles. So the gap is an **architecture/design choice**
(markers = trackable collectibles, avoid clutter), not a fixable per-item bug. Closing
it toward MapGenie means relaxing those filters — a behavior change, not a fix.

**3. Whole classes intentionally not mapped (scope, not bugs).**
The mod is a collectible/loot tracker, not an atlas or walkthrough:

- **Locations** (POI): Divine Tower, Martyr Effigy, Landmark, Dungeon/Portal/
  Elevator/Hidden Passage entrances, Evergaol, Minor Erdtree, … — none wired.
- **NPCs**: Character, Ghost, Merchant, Trainer — none wired.
- **Enemies** (non-boss): Elite Enemy, Enemy — none wired.
- **Other** (guide annotations): Lore, Miscellaneous, Puzzle, Quest-steps, Stone
  Cairn, Summoning Sigil — none wired.

(The mod *does* wire the placed world-features MapGenie files under Locations:
Site of Grace, Stake of Marika, Spiritspring Jump, Imp Seal Statue — all ✅.)

## Internal drift: 128 baked-but-invisible markers

Separate from the MapGenie comparison, **128 markers exist in `goblin_map_data.cpp`
but never display in-game** — they sit on a source map that the baked `LEGACY_CONV`
can't project onto the overworld (60/61), so they're injected with a dungeon `areaNo`
and never rendered. (The coverage tool already excludes these from its area counts,
so the MapGenie numbers above are not inflated by them.)

| Source map | markers | status |
|---|--:|---|
| **m35_00 = Leyndell, Ashen Capital** | **120** | **FIXED (commit afb5dec)** |
| m19_00 | 5 | irreducible |
| m45_00 | 3 | irreducible |

The big one was the **Ashen Capital** (Leyndell after the Erdtree burns, late game).
The game's `WorldMapLegacyConvParam` projects it transitively:
`m35 → area 11 (Leyndell) → area 60 grid(44,50)` (overworld), but the mod's bake
kept only **direct** `→ 60/61` entries (single hop) and dropped the `m35 → 11` link.
**Fixed:** `generate_legacy_conv_cpp` now follows `area → area` chains and composes
the translations down to 60/61 (LEGACY_CONV 92→94 entries). The 120 Ashen Capital
markers now land at Leyndell on the overworld; baked-but-invisible dropped 128→8.
(m19/m45's 8 markers have no chain to 60/61 at all → irreducibly invisible, like
EMEVD-only gives.)

## Cross-cutting signal: DLC is weaker than base

Where base matches, the DLC tends to under-cover: Bosses DLC −11, Smithing −35,
gathering −123, Equipment negative throughout. Worth a dedicated look — distinct
from the loot-source gap.

## What to do

1. ✅ **Generate the vanilla profile** — done. Equipment negative is present in
   vanilla too → mod-side (design/source-gap), confirmed not an ERR edit.
2. ✅ **Drill the negative** — done. Root cause = the four collectible-only filters in
   `generate_loot_massedit.py:821–833` (above). Design choice, not a bug.
3. **Decide scope** on the unwired classes — likely "leave as-is" (loot tracker),
   except maybe a small high-value Locations layer (Divine Towers + notable
   landmarks) if desired.
4. **Open**: vanilla DLC column reads 0 for several key-items (Cookbook/Map Fragment/
   Bell Bearing/Crystal Tear) though Scadutree came through — re-check the vanilla DLC
   extraction before trusting the area-61 vanilla numbers.

Bottom line: **the mod is not broadly buggy.** Placed content matches MapGenie; the
gaps are an architectural limit (un-placed loot) plus deliberate scope.
