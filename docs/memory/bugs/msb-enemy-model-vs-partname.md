---
name: msb-enemy-model-vs-partname
description: "Never derive an enemy's chr model from its MSB part NAME. An enemy randomizer swaps ModelName + NPCParamID while keeping the original part name, so the prefix is the creature that used to be there — 84.7% divergence measured on Randomizer v0.11.4."
metadata:
  node_type: memory
  type: project
---

# The MSB part name is NOT the enemy's model

**Rule.** The chr model of a placed enemy comes from the MSB part's `modelIndex` (u32 @ `part+0x14`)
resolved against the **MODEL section** names — never from the `c####_9000` prefix of the part NAME.
`msbe::Enemy::modelName` / `DiskEnemy::modelName` carry it; `enemy_model_id()` in `map_entry_layer.cpp`
is the single accessor (prefers `modelName`, falls back to the name prefix only when unresolved).

**Why it matters.** An enemy randomizer swaps a creature in place by rewriting `ModelName` +
`NPCParamID` and **keeping the original part name**. So the name prefix is the creature that USED to
stand there. Measured 2026-07-27 on vanilla + Randomizer v0.11.4:

```
459 tiles, 22959 placed enemies
part-name prefix != ModelName : 19439  (84.7%)
```

For 5 enemies out of 6 every model-keyed lookup was reading the wrong creature — the bestiary codex
(`resolve_enemy_name` tier 2) and the NpcName boss band (tier 3) both key on the model. This is the
upstream cause of a whole family of wrong enemy/boss names, and it silently makes the tier-3 band
"find" bosses that are not there (see [[mod-agnostic-boss-markers]]).

Live before/after on one underground tile (`vmap ename 12 1 0`), same install:

| | tier 0 | tier 1 | tier 3 | tier 4 |
|---|---|---|---|---|
| part-name model | 224 | 1 | **98** | 4 |
| real `ModelName` | 306 | 1 | **21** | 2 |

The tier-3 false-boss count fell 98 → 21 in that tile alone (model c3320: 52 placements → 1).

**The same trap already existed for assets** and was solved there first: `Asset::modelName` exists
because ERR substitutes a gather node's model while keeping its vanilla part name, and GEOF
collected-graying has to bucket by the real model. The enemy path simply never got the same treatment
— worth checking whenever a new MSB part type is parsed.

**Tier 4 is immune** (it keys on `EntityID`, not the model), which is exactly why the Gatefront boss
came out correctly named while everything model-derived around it did not. Live confirmation after the
fix: `part='c3251_9000' npc=47701242 model=c4770 tier=4` — the part name still says c3251, the model
we now read is c4770.
