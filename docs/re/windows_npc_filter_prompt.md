# Quest-NPC marker filter — audit fixes (Windows, ERR profile)

**Status: DONE on `feat/quest-npc-filter` (off `master`).** All three audit problems
fixed, regenerated, and verified against live ERR data. This doc records the outcome
(it began as the audit task prompt).

## What was wrong

The v1 filter (`teamType ∈ {0,1,2,26}`, `EXCLUDE_MODELS={c1000}`, `nameId>0`,
`EntityID>0`) had three problems, all confirmed per-placement against the MSBs +
NpcParam (`tools/_qnp_diag.py`, throwaway):

- **Missing static merchants** — dropped by teamType or by the c1000 model exclude.
- **False positives** — map-event objects placed under a *non-c1000* model leaked.
- **Data anomaly** — a placement with a stale nameId that has no NpcName entry.

## The fix — hybrid filter (teamType + allowlist exceptions + denylist)

Kept the friendly-team base, dropped nothing wholesale; added two nameId lists:

```python
FRIENDLY_TEAM_TYPES = {0, 1, 2, 26}
EXCLUDE_MODELS      = {'c1000'}          # bulk menu/system objects, FORCE-exempt

DENY_NAME_IDS = {                        # objects/enemies that leak through
    170000,                  # Altar of Anticipation (8 leaked via model c4300)
    160100, 160200, 160500,  # Church / Smithing Table / Cathedral of Dragon Communion (c0100)
    133200,                  # no NpcName FMG entry -> renders blank
    110100,                  # Torrent (the mount) — borderline, dropped
    120000,                  # Rennala (boss) — borderline, dropped
} | set(range(121601, 121611))           # "Menu"/"Lord's Journey" menu family

FORCE_NAME_IDS = {                       # static merchants, included regardless of team + model
    160000,            # Twin Maiden Husks (Roundtable; team 0 BUT model c1000)
    121800, 121810,    # Asimi, Silver Tear / Eternal King (team 8)
    135200,            # Preceptor Miriam (team 27)
    140601,            # Ancient Dragon Florissax (DLC)
}
# 9-digit nameIds (>= 900000000) = generator-name enemies (e.g. Equilibrious Rat
# 904080600); dropped via _is_generator_enemy_name().
```

`friendly_ids` now = `name>0 AND name∉DENY AND not 9-digit-enemy AND (team∈FRIENDLY OR
name∈FORCE)`; the model exclude gained a `name_id ∉ FORCE_NAME_IDS` exception.

### Root causes found (per-placement diagnostic)

| nameId | name | why it was wrong | fix |
|---|---|---|---|
| 160000 | Twin Maiden Husks | team 0 ✓ but **model c1000** → killed by model exclude | FORCE |
| 121800 | Asimi, Silver Tear | **team 8** (outside friendly) | FORCE |
| 135200 | Preceptor Miriam | **team 27** (invader team) | FORCE |
| 170000 | Altar of Anticipation | 10 placements: 2×c1000 (caught) + **8×c4300** (leaked) | DENY |
| 160100/160200/160500 | Dragon Communion / Smithing | **model c0100** (not c1000) | DENY |
| 121601–121610 | "Menu" / "Lord's Journey" | 121608 placed on **c0000**, not c1000 | DENY range |
| 133200 | (no FMG) | unnamed c0000, stale nameId → blank label | DENY |
| 904080600 | Equilibrious Rat | DLC enemy, 9-digit generator-name | DENY (≥900000000) |

(Asimi Eternal King 121810 and Florissax 140601 have 0 live placements — harmless in
the FORCE set.)

## Result

- **Count: 363 → 344** markers.
- **Added:** Twin Maiden Husks (2), Asimi (1), Preceptor Miriam (2).
- **Removed:** Altar of Anticipation (8), Dragon Communion ×3, Lord's Journey (1),
  Equilibrious Rat (4), nameId 133200 blank (4), Torrent (1), Rennala (2).
- Kalé still present (1). Hero-tomb statues (iconId 440) intact at 16 (no row-id
  collision). `goblin_map_data.cpp` diff is **WorldQuestNPC-only** (9315 → 9296
  Category rows); `goblin_location_alt.cpp` only its 9400xxx entries.

## Files

`tools/generate_quest_npcs.py` (DENY/FORCE lists + hybrid `friendly_ids` + model
exclude exception); regenerated `src/generated/goblin_map_data.cpp`,
`src/generated/goblin_location_alt.cpp`, `data/massedit_generated/World - Quest NPC.MASSEDIT`.

## Remaining

Runtime-test: enable `show_quest_npc`; confirm Twin Maiden Husks now shows at the
Roundtable's overworld projection, the Altar of Anticipation / Dragon Communion
markers are gone, and no blank-label markers remain. Borderline calls (Torrent,
Rennala dropped) can be revisited if wanted.
