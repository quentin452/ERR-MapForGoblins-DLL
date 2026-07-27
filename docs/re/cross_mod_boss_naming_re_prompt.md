# RE prompt — mod-agnostic boss enumeration (name + world position)

Status: **SOLVED (2026-07-27)** — see `cross_mod_boss_naming_re_findings.md`. No Ghidra was needed:
the answer is the EMEVD instruction `2003[011] HandleBossHealthBar(state, entityId, slot, nameId)`
joined with the MSB placement + `GameAreaParam`; 254 named+positioned bosses on vanilla+randomizer,
validated in-world against the game's own boss health bar. The leads below are kept for the record —
`NpcParam` has **no** GameAreaParam id, and `GameAreaParam` alone has no name and no position.
**Implementation in the DLL is NOT started** (see the findings' "Implementation notes").

Originally opened 2026-07-23, deferred from the bug-5 work.

## Problem

MapForGoblins draws a **WorldBosses** marker category. We need, on **ANY** install (vanilla, randomizer,
Convergence, ERTE, ERR), the set of field bosses / minibosses as **(display name, world position, a stable
per-boss identity)** so each boss gets ONE correctly-named marker at the right place.

## Why the current sources are inadequate

1. **ERR `WorldMapPointParam.textId2 == 5100` pins** (`build_live_bosses`, `map_entry_layer.cpp`): these are
   ERR-authored map pins — **absent on vanilla/randomizer** (0 rows). ERR-only.
2. **Tier-3 NpcName boss band** (current mod-agnostic fallback, `enemy_display_name(...,&tier)==3`): names
   from the live MSB enemy scan. It WORKS (names show) but is imperfect: `tier` is a NAME-SOURCE tier, not
   an "is-boss" signal, and the model→name mapping produces **duplicates / wrong names** (observed: three
   "Mimic Tear" from the enemy scan where the map should show the distinct instances, and clone-model
   bosses collapse to one name). It also can't tell a real field boss from a strong regular mob reliably.
3. **The item-search name↔position correlation** (user-suggested reuse): that list is for **POIs**
   (`WorldMapPointParam` landmark rows), **not entities** — no boss-entity coverage. Can't reuse.

So we need the GAME's own boss↔name↔position association, read live, mod-agnostic.

## Leads to chase (in Ghidra + live CE on the running game)

- **`GameAreaParam`** — the boss-arena / boss-HP-bar param. Rows key a boss encounter (bgm, HP-bar name id,
  reward). If a boss's `NpcParam`/enemy carries a `gameAreaParamId` (or an EMEVD/region links entity→
  GameAreaParam), that's a clean "this enemy IS a named boss" signal + a name id independent of NpcName.
  Find: does `NpcParam` (or `EnemyIns`) expose a GameAreaParam id? Which field?
- **`CSFeManImp` boss HP bars** — `src/re_signatures.hpp:275` notes `CSFeManImp+0x59F0 = EntityHpBar
  entityHpBars[8]; +0x5BF0 = BossHpBar bossHpBars[3]`. That's the LIVE nearby-boss set (name + entity), used
  by the in-world enemy-name work ([[combat_enemy_list_structure_re_prompt]]). Only covers bosses near the
  player — NOT a map-wide enumeration — but it's the ground-truth "what the game calls a boss + its name",
  useful to VALIDATE any static source.
- **`NpcParam` boss flags** — is there a field marking "boss" (HP-bar type, deathblow, `disableParryAttack`,
  a boss-specific spEffect)? A per-NpcParam boss bit + the MSB placement pos would give map-wide bosses
  without name-band guessing.
- **EMEVD boss-defeat flags** — bosses have a defeat event flag (the same one `clearedEventFlagId` uses for
  the cleared-check). The EMEVD/`GameAreaParam` cross-ref could enumerate boss entities. We already parse
  EMEVD (`build_disk_emevd`); check whether a boss-defeat instruction pattern yields entity + flag.
- **Bestiary / codex** — the codex (`kTutorialBand` tier-2 in `goblin_enemy_names.cpp`) has boss entries;
  can it be walked to a canonical boss list + names? Its keying is by MODEL (the clone-name trap source).

## Deliverable

A mod-agnostic function (host or render, called from `build_live_bosses` as the PRIMARY source, ERR
`textId2==5100` demoted to an additive layer):

```
for each boss encounter the game recognizes → { entityId | stable key, display name (FMG id or string),
                                                 world position (area/grid/pos), cleared event flag }
```

- Prefer an **FMG id** for the name (routes through `lookup_text_utf8`, no `Marker::live_name` hack + no
  synthetic name_id — see [[mod-agnostic-boss-markers]]).
- Dedup by encounter, not by model (so clone bosses like Godefroy/Godrick don't collapse; recurring bosses
  like Mimic Tear keep their distinct locations).
- Validate live against `CSFeManImp` bossHpBars: walk near a known boss, confirm the enumerated name/pos
  matches the game's own HP-bar label.

## Pinning

If a new structure/offset is found, add an AOB to `src/re_signatures.hpp` (logged PASS/FAIL at boot) and a
`*_re_findings.md` here. Related: [[combat_enemy_list_structure_re_prompt]],
[[mod-agnostic-boss-markers]] (docs/memory/bugs), `docs/re/README.md` (coverage map).
