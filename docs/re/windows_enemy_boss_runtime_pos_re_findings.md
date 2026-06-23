# Findings — runtime enemy / boss world position (dungeon-boss DRIFT)

Companion to `windows_enemy_boss_runtime_pos_re_prompt.md`. Sections filled as work lands.

---

## T3 — Is the vanilla field-boss param row in overworld frame? → **NO (verdict: FAILS)**

**One-line:** The `WorldMapPointParam` field-boss row (`textId2==5100`) for a dungeon boss is
authored in the **same dungeon-internal frame** as the MSB entity we already bake — not the
overworld frame. Swapping the baked source from MSB-entity-pos to WMP-row-pos is a **no-op**.
The zero-RE fix the prompt hoped for does not exist.

**Method:** `tools/diag_t3_bossframe.py` (ERR profile). Loads live `WorldMapPointParam`, indexes
the 5100 rows by `clearedEventFlagId`, joins against the already-baked `data/boss_list.json`
(which carries the MSB-entity pos + the same flag), and prints both frames side by side.

**Data (215 bosses, all flags matched 1:1):**

| metric | count |
|---|---|
| boss_list entries | 215 |
| 5100 WMP rows w/ flag | 215 |
| dungeon bosses (entity area ∉ {60,61}) | 106 |
| …whose 5100 row IS overworld (60/61) | **0** |
| …with no 5100 row at all | **0** |

For every dungeon boss the WMP row's `areaNo` equals the dungeon area (10/11/12/30/31/32/…) and
its `(posX,posZ)` equals the dungeon-internal `(x,z)` we bake — frequently byte-identical
(Scaly Misbegotten m32 `83.3,-25.4` == row; Stonedigger Troll m32 `27.7,-0.8` == row; Runebear
m31 `-146.9,-144.9` == row). Overworld bosses (60/61): row ≈ entity, same frame.

**Implication — this is the valuable result, not just a dead branch:**
1. The drift is **purely in the projection**, never in the choice of source position. Both static
   sources we own (MSB entity *and* the param row) are in the dungeon's own frame; neither is
   pre-resolved to the overworld.
2. By extension a **runtime ChrIns read (T1/T2) would report the same dungeon-frame coords** — so
   this independently **reinforces the prior "runtime is a DEAD END" conclusion** rather than
   leaving it open. The decision tilts toward design-fix **A (collapse-to-entrance)** over both
   the WorldChrMan enumeration RE and runtime reading.

**Remaining gate before committing to the design fix (one in-game observation):** these 106
dungeon 5100 rows are ERR additions. Does the **live ERR engine** draw their boss icons at the
correct dungeon entrance on the world map? If YES → the engine has a correct dungeon-frame →
overworld WMP projection worth replicating (RE the WMP-render path, *not* WorldChrMan). If it
drifts them too → the collapse-to-entrance design fix is the only path.

**Incidental bug:** `generate_boss_list.py` mis-matches **Flamelost Knights** — baked entity is
`m31_08` but its 5100 row is area 16; the cross-tile matcher grabbed an entity from the wrong map.
Both are dungeon-frame so it didn't change the T3 verdict, but it's a real placement error.

---

## T1 — Enemy ChrIns enumeration off WorldChrMan
_Not started — T3 result tilts away from this path; revisit only if the in-game gate above shows
the engine itself projects WMP dungeon rows correctly AND a runtime overworld-frame coord is
needed._

## T2 — Live read of one dungeon boss (the decisive runtime experiment)
_Not started. T3 already predicts the outcome (runtime = same dungeon-frame coords). Run only to
confirm if the design decision needs the hard datum._

## T4 — Unloaded enemies
_Constraint stands a priori: an un-streamed enemy has no ChrIns → no runtime pos; any runtime fix
only works while the player is inside the dungeon. Moot if the design fix is chosen._
