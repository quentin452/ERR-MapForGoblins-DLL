# Task: tune + generate the Quest-NPC layer (Windows, ERR profile)

You are on a **Windows** box with the **ERR-MapForGoblins-DLL** repo cloned and the
**err** profile already building (VS2022 + Python deps + `tools/config.ini` with a valid
`err_mod_dir`, ERR mod's `regulation.bin` + `map/MapStudio/*.msb.dcx` present). The data
pipeline runs here; it cannot run on Linux (needs pythonnet + SoulsFormats + the mod).

## Background

New feature (Thread 1): a **named friendly NPC + merchant** location layer for quest
navigation. The DLL + pipeline wiring is already committed on the Linux side
(`Category::WorldQuestNPC`, config `show_quest_npc`, `CATEGORY_MAP`, a `build_pipeline.py`
Stage, `PRESERVE_FILES`). The generator `tools/generate_quest_npcs.py` exists but its
**filter is untuned** — two constants near the top are guesses and must be set from real
data on this machine:

- `FRIENDLY_TEAM_TYPES = {1, 2, 6, 7, 8}` (line ~54) — GUESS. The friendly-NPC teamType
  taxonomy must be verified.
- `ICON_ID = 374` (line ~59) — placeholder (hostile-NPC sprite, safe-renders). A distinct
  friendly/quest icon must be chosen.

How the generator works (read it — it's short, ~230 lines):
1. From `NpcParam`: keeps NPC IDs whose `teamType ∈ FRIENDLY_TEAM_TYPES` **and** `nameId > 0`
   (named characters/merchants; ambient creatures have no name).
2. Scans all MSBs for `Parts.Enemies` whose `NPCParamID` is in that set and `EntityID > 0`
   (placed, not script-spawned), dedups per (map, rounded x/z).
3. Emits `data/massedit_generated/World - Quest NPC.MASSEDIT`; label = `nameId + 700000000`.

## Steps

### 1. Tune `FRIENDLY_TEAM_TYPES`

```bat
python tools\generate_quest_npcs.py --inspect
```

Prints `teamType -> placed named-NPC count` with up to 8 samples each
(`npc / nameId / model / partName @ map`). For each teamType, judge from the samples
whether it's **friendly NPCs/merchants** (Kalé, Twin Maiden Husks, Roderika, Gostoc, Boc,
Blaidd, Iron Fist Alexander, …) or **enemies/bosses**. Set `FRIENDLY_TEAM_TYPES` to only
the friendly ones.

- Invader teamTypes `{24, 27}` belong to `generate_hostile_npcs.py` — do NOT include them.
- Exclude any teamType whose samples are clearly enemies or bosses (bosses are a separate
  layer).
- Use `model` (chr id) + `partName` to disambiguate — a merchant/quest model in the sample
  is a strong friendly signal.

### 2. Pick `ICON_ID`

Choose a **distinct** friendly/quest worldmap icon from the atlas (NOT 370=grace, NOT
374=hostile — those collide visually). Confirm it actually renders in-game before locking
it. **User requirement:** this family must be visually distinct and must NOT be clustered
when clustering ships — so the icon has to read clearly on its own.

### 3. Generate + sanity-check

```bat
build.bat generate
```

(err profile — no `--vanilla/--convergence/--erte`.) The Stage emits the MASSEDIT and
re-bakes `src/generated/goblin_map_data.cpp` with `Category::WorldQuestNPC` rows. Check:

- **Count is sane** — expect a few hundred markers (named NPCs aren't numerous). Thousands
  = enemies leaking in → re-tune step 1. Single digits = set too narrow.
- **Names resolve** — `textId1 = nameId + 700000000` present on rows.
- **Spot-check a known NPC** — e.g. Kalé at the First Step site of grace; confirm position
  lands on the right map area.

### 4. Deliver

Return:
1. The regenerated **`src/generated/goblin_map_data.cpp`** (+ the new
   `data/massedit_generated/World - Quest NPC.MASSEDIT`).
2. The final **`FRIENDLY_TEAM_TYPES`** and **`ICON_ID`** values used (so the Linux side
   commits the tuned generator).
3. The **full `--inspect` dump** (the teamType breakdown). Required — it's the evidence for
   the chosen set, and lets the Linux side narrow further if counts look noisy.

Do NOT push a branch; `src/generated/` is committed but the tuned constants land via the
Linux box. Zip / paste back the two files + the values + the inspect dump.

If a teamType is ambiguous (mix of friendly + hostile samples), report it rather than
guessing — list its samples so we decide together.
