# Quest-NPC layer — tuning + generation (Windows, ERR profile)

**Status: DONE on `feat/cluster-glyph` (off `feat/clustering`).** Tuned, generated,
baked, and spot-checked on this box. This doc records what shipped (the original
task prompt assumed only "set 2 constants" — in reality the generator had never run
and carried 3 API bugs + a row-id collision, all fixed below).

## What the layer is

A **named friendly NPC + merchant** location layer (Thread 1) for quest navigation:
one marker per placement of a named, placed (`EntityID>0`) NPC whose NpcParam
`teamType` is friendly-dominated, labelled `nameId + 700000000`. DLL/pipeline wiring
(`Category::WorldQuestNPC`, `show_quest_npc`, `CATEGORY_MAP`, the `build_pipeline.py`
stage) was already committed; `tools/generate_quest_npcs.py` is the generator.

## Tuning result (from `--inspect` + full name resolution on ERR data)

`teamType` is **per-placement combat state, not a per-character trait** — the same
friendly NPC (Moore, Leda, Millicent, Rogier, Nepheli…) has placements in several
teams (friendly phase / turns-hostile / invades-you). So no team is purely friendly;
the tuned set is the teams **dominated** by named merchants + questline NPCs:

```python
FRIENDLY_TEAM_TYPES = {0, 1, 2, 26}   # was {1,2,6,7,8} (a wrong guess: 6/7 are bosses)
```
- **0** — base-game friendly NPCs (Gideon, Twin Maiden Husks, Hewg, Enia, Sellen,
  Rennala, Roderika, Blaidd, Gostoc, …). 918 raw placements but **685 are model
  `c1000` "Menu"/system objects** ("Altar of Anticipation", "Trial of Recollection", …).
- **26** — the cleanest: merchants + questline NPCs (Nomadic/Hermit/Isolated Merchants,
  Patches, Alexander, Boc, Goldmask, Leda, Ansbach, Hyetta, Jar-Bairn, Ranni, …).
- **2** — base quest NPCs (Nepheli, Bernahl, Blaidd, Dane, Melina, Shabriri, …).
- **1** — DLC quest NPCs (Hornsent, Ansbach, Moore, Freyja, Leda, Dane).
- Excluded: **6** (field bosses/mimics), **7** (great-enemy bosses), **24/27**
  (invaders), **48/33/9/52** (misc enemies).

```python
EXCLUDE_MODELS = {'c1000'}   # menu/system objects (the 685 "Menu" rows), not NPCs
```

**Count: 363 markers (all named)** — squarely in the doc's "few hundred" target.
Spot-checked: Kalé ✓, Hewg, Roderika, Blaidd×10, Patches×7, Sellen×8, Leda×4, Ranni,
Boc ✓. (Twin Maiden Husks is absent — they're only at Roundtable Hold, an interior hub
reached via the grace menu, not a navigable overworld location; correct to omit.)

## Generator fixes (it had never run)

1. `read_param`: `f.Bytes` → `f.Bytes.ToArray()` (SoulsFormats returns `Memory<byte>`).
2. `PARAM.Read(tmp)` direct call → reflected `_param_read.Invoke(...)` (pythonnet can't
   resolve the string overload), and key paramdefs by `ParamType`, not the param name.
3. **Row-id collision**: `row_id` base `9300000` collided with hero-tomb-statues
   (also 9300000) — `generate_data` keys rows by id (last writer wins), so the 363
   quest rows clobbered all 16 hero-tomb markers (iconId 440 → 0). Moved to **9400000**
   (free; loot=9000000/9100000, hostile=9200000, hero-tomb=9300000).

## Icon (ICON_ID = 443)

A dedicated friendly "person bust" glyph (blue), synthesised by `build_vanilla_gfx` as
the 3rd appended frame after anon (441) + cluster (442) — same clone-shape-172 +
raster-embed trick. `tools/make_quest_npc_icon.py` draws the 160px PNG
(`assets/badges/quest_npc_glyph.png`). ERR-only layer (offset 0) → iconId 443 directly.
All 4 profiles rebaked: frame at index 442 (443 frames) for vanilla/erte/err, index
850 (851 frames) for convergence; shape 1101 verified embedded.

## Files

- `tools/generate_quest_npcs.py` — tuned constants, c1000 filter, 3 fixes, row-id 9400000.
- `tools/make_quest_npc_icon.py` + `assets/badges/quest_npc_glyph.png` — the glyph.
- `tools/build_vanilla_gfx.py` — `add_quest_npc_icon` + wiring (shape 1101, frame 442).
- `assets/menu/02_120_worldmap_{vanilla,erte,err,convergence}.gfx` — rebaked.
- `src/generated/goblin_map_data.cpp` (+363 WorldQuestNPC rows) + `goblin_location_alt.cpp`
  (+28 location-alt entries) + `data/massedit_generated/World - Quest NPC.MASSEDIT`.

## Remaining

Runtime-test in-game: enable `show_quest_npc`, confirm the blue person glyph shows on
named NPCs (e.g. Kalé at the Church of Elleh / First Step), and that hero-tomb statues
(iconId 440) still render (collision regression check). Non-ERR profiles carry the
frame but the layer itself is ERR-only for v1.
