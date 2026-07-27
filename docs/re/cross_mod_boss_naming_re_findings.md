# Findings — mod-agnostic boss enumeration (name + world position + identity)

Answers `cross_mod_boss_naming_re_prompt.md`. Status: **SOLVED (2026-07-27)** — no Ghidra needed.
Derived + live-validated on a **vanilla + Elden Ring Randomizer v0.11.4** install (Windows dev box),
the exact case the prompt was written for.

## Verdict in one line

The game's own per-encounter boss table is the EMEVD instruction **`2003[011]`
`HandleBossHealthBar(state, entityId, hpBarSlot, nameId)`**. Joined against the MSB enemy placement
(entity → position) and `GameAreaParam` (entity → defeat flag), it yields the prompt's full
deliverable — **name + world position + stable identity + cleared flag — from the ACTIVE install's
own files**, for every boss the game raises a health bar for.

## The chain

| step | source | key | gives |
|---|---|---|---|
| 1 | `event/*.emevd.dcx`, opcode `2003[011]` | `(map, entityId)` | FMG `nameId` (NpcName band `9xx`), HP-bar slot |
| 2 | `map/MapStudio/*.msb.dcx` `Parts.Enemies` | `EntityID` | world position, `NPCParamID`, chr model |
| 3 | `GameAreaParam[rowId]` | **rowId == boss entityId** | `defeatBossFlagId` (cleared check), `bonusSoul`, `bossMapAreaNo/BlockNo` |
| 4 | `msg/<lang>/…NpcName*.fmg[nameId]` | `nameId` | display string |

Reproduce: `python tools/_probe_boss_enum.py <game_data_dir> [out.json] [lang]`.

Measured on the randomizer install:

```
EMEVD 2003[011]: 255 boss bars, 245 distinct nameIds
MSB            : 25291 placed enemies with an entity id
GameAreaParam  : 216 rows
JOINED 254/255 with a position, 254/254 with a name
  no MSB placement: 1  [('m60_41_33_00', 1041330800, 904133540)]
  no GameAreaParam row: 39
  per area: 60=89, 61=34, 30=25, 31=22, 32=14, 12=12, 13=8, 16=5, 11=5, 41=5, … (25 areas)
  names shared by >1 encounter: 23  (Death Cavalry x4, Night's Cavalry x3, Godskin Champion x3, …)
```

254 named + positioned bosses vs the **217** ERR-only `WorldMapPointParam textId2==5100` pins, with
per-ENCOUNTER identity instead of per-model. The 23 shared names are legitimately recurring bosses
(4 distinct Death Cavalry at 4 distinct positions), not the collapse/duplication of the tier-3 band.

## How the opcode was found (empirical, no emedf guessing)

Scanned all 485 `.emevd.dcx` for instructions whose ArgData contains an int in the vanilla NpcName
boss band `[900000000, 910000000)` and tallied by `bank[index]`:

```
2003[011]  x369   [1, 10000800, 0, 907770040]     <- the boss health bar (winner)
2000[006]  x177   [0, 900005610, 10001680, …]     <- InitializeCommonEvent, 9xx = a common-func id
2000[000]  x96    [0, 1700016, 12010850, 907770190, 12]  <- init passing entity+nameId through
3[008]     x1                                      <- noise
```

`2000[000]`/`2000[006]` are false positives of the band test; `2003[011]`'s arg layout is
unambiguous (`args[1]` is always a real MSB entity id, `args[3]` always a NpcName id).

## Live validation (RPC, 2026-07-27, build `Jul 27 2026 09:52:39`)

The prompt asked for a live check against the game's own boss HP bar. Done at **Gatefront,
tile area60 grid(42,36)**, the tile next to the player:

- offline chain says: `m60_42_36_00` entity `1042360800` → **"Black Tree Kindred Sentinel"** @ (-12.1, 89.0, 46.8)
- the mod's CURRENT tier-3 source says (`vmap ename 60 42 36`):
  `part='c3251_9000' npc=47701242 model=3251 grid(42,36) pos(-12,47) -> name='Tree Sentinel' tier=3`
- **the game's own boss health bar reads "Black Tree Kindred Sentinel"** (user-confirmed in-world).

So the EMEVD chain is right and the tier-3 band is wrong on this install. Root cause, read live:
`param_get NpcParam 47701242 0x0c s32` → **`nameId = 0`**, so `enemy_display_name` falls through
tier 1 → the `900000000 + model*1000` band, which names the **vanilla model** (c3251 = Tree
Sentinel), not the randomized encounter. **On any mod that reskins/reassigns bosses, tier-3 is
wrong by construction, not just imprecise.**

`GameAreaParam` was confirmed live too — the offline dump is byte-accurate and the row keying holds
for overworld bosses:

| live read | value |
|---|---|
| `GameAreaParam[10000800] +0x04 u32` (`bonusSoul_single`) | 20000 ✓ matches disk |
| `GameAreaParam[10000800] +0x44 u32` (`defeatBossFlagId`) | 10000800 ✓ (== rowId == entity) |
| `GameAreaParam[1042360800] +0x44 u32` | 1042360800 ✓ overworld row exists |

## What the prompt's other leads turned out to be

- **`NpcParam` → GameAreaParam id: does not exist.** No field named `gameArea*` in ANY of the 194
  paramdefs. `NpcParam`'s only boss-ish field is the bit `isSoulGetByBoss` (311/7548 rows) — far too
  broad to be an "is a field boss" signal.
- **`GameAreaParam` alone is insufficient** — it has the encounter set and the defeat flag but **no
  name and no position** (`foundBossTextId`/`notFindBossTextId` are multiplayer sign texts). It is
  the right ADDITIVE layer (cleared-state graying), not the primary source.
- **`CSFeManImp` bossHpBars** (`src/re_signatures.hpp:275`) was not needed: the same name the bar
  displays is authored statically in the EMEVD, so the static read gives the bar's own string.
- **Bestiary / `TutorialParam` codex (tier 2) stays model-keyed** — same clone/reskin trap as tier 3.

## Implementation notes (not started)

Everything needed is already parsed by the DLL — this is a wiring job, not new RE:

- EMEVD parsing exists (`build_disk_emevd` / `build_disk_emevd_markers`, `map_entry_layer.cpp`) —
  add the `2003[011]` extraction to build `{entityId → nameId}`.
- MSB enemy placements exist (`disk_enemies`, the same scan `build_live_bosses` already walks) —
  join on `EntityID`.
- The name is a plain **FMG id**, so it routes through the normal text path: no `Marker::live_name`
  hack and no synthetic `name_id` (see [[mod-agnostic-boss-markers]]).
- Demote to additive layers: ERR `textId2==5100` pins, then the tier-3 band as the last-ditch
  fallback for a boss with no health-bar event.
- **Two consumers, one root cause.** Besides the map's `build_live_bosses`, the world-space 3D enemy
  tag (`config::nameEnemyBosses`, fed via `enemy_display_name` tier 3) shows the same wrong vanilla
  model name in-world — user-observed 2026-07-27 on the same Gatefront boss. Source the name by
  **entityId** in the shared resolver (`goblin_enemy_names.cpp`) and both are fixed at once; note the
  new table is keyed by entity, while today's resolver signature takes `(npcParamId, model)`.

## Caveats

- 1/255 bars has no MSB placement (`m60_41_33_00` e=`1041330800`) — an event-spawned boss; it needs
  a fallback position (its GameAreaParam `bossMapAreaNo/BlockNo`, or the map tile itself).
- 39/254 bosses have no `GameAreaParam` row → the defeat-flag layer must be optional per boss.
- A boss bar can be raised from a map other than the one the entity is placed in; the probe falls
  back to a unique-entity lookup across all MSBs (1 candidate = accept).

Related: [[combat_enemy_list_structure_re_prompt]], `docs/memory/bugs/mod-agnostic-boss-markers.md`,
`docs/re/windows_enemy_name_runtime_source_re_findings.md` (§2 named the EMEVD `HandleBossHealthBar`
linkage first — this finding turns that observation into a full enumeration).
