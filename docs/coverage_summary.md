# MapForGoblins vs MapGenie — coverage synthesis

Cross-checked the mod's marker counts (ERR profile) against MapGenie's full
category taxonomy for both maps. Per-type tables: [coverage_base.md](coverage_base.md)
(Lands Between) and [coverage_dlc.md](coverage_dlc.md) (Shadow Realm). Regenerate
with `tools/coverage_vs_mapgenie.py`.

**Caveat:** MapGenie maps **vanilla** Elden Ring; this is the **ERR** profile. So a
negative (mod < MapGenie) is a *candidate* gap, not proof — generating the vanilla
profile (`build.bat --vanilla generate`, see [windows_generate_prompt.md](windows_generate_prompt.md))
gives the clean same-game baseline.

## The picture in three buckets

**1. Physically-placed content → covered well (✅ matches or exceeds MapGenie).**
Things with an MSB position. Close matches validate both the tooling and the
coverage:

| Type | mod / MapGenie (base) |
|---|---|
| Rune Arc | 63 / 63 ✅ |
| Deathroot | 9 / 9 ✅ |
| Map Fragment | 19 / 19 ✅ |
| Bosses (all) | 179 / 170 ✅ |
| Invasion | 37 / 36 ✅ |
| Smithing Stones | 469 / 446 ✅ |
| Gathering materials | 1627 / 1009 (mod ≫, ERR/extensive nodes) |

**2. Loot with no physical placement → systematic negative (the one real concern).**
Equipment is the headline: nearly every type is 20–60% below MapGenie.

| Type | mod / MapGenie (base) |
|---|---|
| Weapon + Shield | 245 / 384 (−139) |
| Incantation | 45 / 101 (−56) |
| Ash of War | 64 / 105 (−41) |
| Talisman | 102 / 122 (−20) |
| Ammunition | 53 / 98 (−45) |

The uniform direction (not mixed) points at a **missing source**, not random ERR
edits: items handed out by **enemy drops / EMEVD events / merchant stock** have no
MSB position, so the mod can't place them. Same root as the 312 `unreachable_msb_lots`
and the original "purple item" investigation. The mod *can* place at an enemy's
position (it does for bosses), so skipping common enemy-drop gear is most likely a
**design choice** (avoid clutter) rather than a fixable per-item bug — to be
confirmed against vanilla.

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
| **m35_00 = Leyndell, Ashen Capital** | **120** | **recoverable** |
| m19_00 | 5 | irreducible |
| m45_00 | 3 | irreducible |

The big one is the **Ashen Capital** (Leyndell after the Erdtree burns, late game).
The game's `WorldMapLegacyConvParam` projects it transitively:
`m35 → area 11 (Leyndell) → area 60 grid(45,52)` (overworld). The mod's bake keeps
only **direct** `→ 60/61` entries (single hop), so it drops the `m35 → 11` link and
the 120 markers vanish. **Fix:** make the LEGACY_CONV generation transitive — follow
`area → area` chains until they reach 60/61 — which would drop the Ashen Capital's
120 markers at Leyndell on the overworld. (m19/m45's 8 markers have no chain to
60/61 at all, so they stay irreducibly invisible, like EMEVD-only gives.)

## Cross-cutting signal: DLC is weaker than base

Where base matches, the DLC tends to under-cover: Bosses DLC −11, Smithing −35,
gathering −123, Equipment negative throughout. Worth a dedicated look — distinct
from the loot-source gap.

## What to do

1. **Generate the vanilla profile** → the bug flag becomes clean (same game). If
   vanilla also shows the Equipment negative, it's mod-side (design/source-gap); if
   vanilla matches, the ERR negatives are just ERR removing content.
2. **Drill one clean negative** (e.g. Equipment Incantation −56, or DLC bosses −11):
   list the specific missing entries and their source → confirms design-vs-bug.
3. **Decide scope** on the unwired classes — likely "leave as-is" (loot tracker),
   except maybe a small high-value Locations layer (Divine Towers + notable
   landmarks) if desired.

Bottom line: **the mod is not broadly buggy.** Placed content matches MapGenie; the
gaps are an architectural limit (un-placed loot) plus deliberate scope.
