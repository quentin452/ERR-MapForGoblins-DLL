# Enemy/Treasure no-bake analysis — the LOOT-slice de-bake matrix (2026-06-25)

Branch `feat/msbe-entity-recover-dummy`. Data-mining pass on the baked loot slice to decide
whether the LOOT tranche can be driven toward a quasi-complete no-bake (treasure live + enemy
live + collectibles) with only a small explicit curated residue.

Source of truth = the ERR provenance regen (`src/generated/goblin_map_data.cpp`,
`MapEntry.loot_source`, commit c31b54b) cross-referenced with `data/items_database.json`
(the offline universe with `partName`/`partBucket`/`npcParamId`). Reproduce with
`tools/` or the `.scratch/` probes; the runtime `[ENEMY-MARKERS]` / `[DEBAKE-GAP]` diags
(gated on `diag_loot_pos`) confirm the same counts live.

> **NUMBER CORRECTION (carried from the handoff):** "25608 enemy drops" is the RAW offline
> `ItemLotParam_enemy` universe, NOT baked markers. The bake has only **119** `LootSource::Enemy`
> markers. The enemy slice is SMALL, not dominant.

---

## 1. MAP_ENTRIES provenance (total 8653 baked rows)

| loot_source | rows | lot-backed | what it is |
|---|---|---|---|
| **Unknown** | 4366 | 29 | non-lot MAP **features** (graces/stakes/NPCs/pieces/nodes) — see §4 |
| **Treasure** | 3639 | 3639 | MSB `Events.Treasure` placements — disk-derivable (§3) |
| **Emevd**   | 529  | 529  | EMEVD scripted grants — no MSB world position (mostly unplaceable) |
| **Enemy**   | 119  | 119  | enemy drops — `ItemLotParam_enemy`, on a ChrIns (§2) |

`Enemy`/`Emevd`/`Treasure` are 100 % lot-backed. `Unknown` is 99 % non-lot (map features).

---

## 2. The 119 enemy markers — fully disk+param-derivable (NOT runtime-walker)

Every one of the 119 `LootSource::Enemy` rows resolves in `items_database.json` by `itemLotId`
and carries an **enemy ChrIns part** (`cXXXX_9YYY`) + an **`npcParamId`**. They are **all
`lotType == 2`** (`ItemLotParam_enemy`).

**Distribution:**
- **By map:** m60 79, m12 10, m61 6, m15 5, m11/m31 3, m16/m35/m30 2, others 1. (Heavily m60 = the
  overworld rune/glovewort farm enemies + roaming merchants.)
- **By category:** GoldenRunesLow 32, GoldenRunes 27, MerchantBellBearings 19, Gloveworts 11,
  Armaments 9, Armour 5, QuestProgression 3, Talismans 3, AshesOfWar 2, LarvalTears 2, others.
- **Kinds:** roaming rune enemies (`c4240`/`c4201`/`c4300` → Golden/Numen's Runes), wraith spirits
  (`c4020` → Ghost Gloveworts), Nomadic/Hermit/Isolated **merchants** (`c3200` → Bell Bearings on
  kill), and named/invasion NPCs (`c0000_9xxx` with a 523xxxxxxx ERR npcParam → Sellen's/Thops's
  Bell Bearing, Varré's Bouquet, Witch's Glintstone Crown…), plus a few bosses (`c3670` Loretta →
  Haligtree Secret Medallion (Right)).

### The no-bake chain (offline-proven, parallels treasure & collectibles)

```
MSB Parts.Enemies[i]                      # resident the moment the tile streams (like treasure parts)
  → position @ Part+0x20                  # the marker position (same transform as treasure/AEG)
  → NPCParamID                            # the part's NpcParam row
  → NpcParam.itemLotId_map  (preferred)   # u32 fields
    else NpcParam.itemLotId_enemy         #   (→ lotType 2)
  → ItemLotParam_enemy[lot] → goods → FMG # identity, live
```

This is exactly the shape of the two no-bake passes that already ship:
- **Treasure:** `Events.Treasure` → partIndex → Part pos + `itemLotId` → `ItemLotParam_map`.
- **Collectible:** `Parts.Assets` (AEG_8xx) → `AssetEnvironmentGeometryParam.pickUpItemLotParamId` → `ItemLotParam_map`.

`tools/extract_all_items.py:533-567` already derives the 119 this way offline (it walks
`msb.Parts.Enemies`, reads `p.NPCParamID`, joins `NpcParam`, takes the enemy Part's name/pos). So
the enemy slice is **disk-derivable in principle** — it needs a **new runtime pass in
`msbe_parser` + `loot_disk`**: parse `Parts.Enemies`, read `NPCParamID`, live-read `NpcParam`
(`itemLotId_enemy`/`itemLotId_map`), emit at the Part position. Not yet built.

### Why the loaded-loot walker is the WRONG tool (question 2 answered: NO)

The `fieldins-pool-registry-re` walker (vtable-scan MapIns `er+0x2a8d6d8` → node `MapIns+0x460`
`{lotId, flag, FieldIns*}`, gate `*(FieldIns+0x50)==lotId`) only populates that node for
**SPAWNED/opened loot** — a post-kill ground drop or an opened chest (§7/§8 of that doc:
`CSMsbEvent==1`, MSB events parsed-then-discarded; pre-kill the drop lot lives only in
`ItemLotParam_enemy` bound to the enemy via `NpcParam`, never in a MapIns loot node). So the 119
enemy lots do **NOT** appear in the walker when the enemy is merely loaded — only transiently
after you kill it. Pre-kill the runtime evidence is the **resident enemy Part** (position) +
`NpcParam` (lot), i.e. the disk-pass chain above, not the walker.

**Verdict:** enemy is no-bakeable, but via a resident-MSB `Parts.Enemies` + `NpcParam` pass, not
via the spawned-loot walker.

### ★ The runtime NOTABILITY signal (datamined 2026-06-25, `tools/datamine_enemy_notability.py`)

A naive "every enemy with a drop lot" emit = the **25608-placement** universe (mostly respawning
clutter: Sliver of Meat ×3480, Thin Beast Bones ×2902, mushrooms…). The bake cures 25608→119 via the
pipeline's ~60 manual `LOOT_CATEGORIES` notability filters — but the game already encodes notability
in a **live-readable** signal, so porting those filters is unnecessary:

The raw `getItemFlagId` counts (`datamine_enemy_notability.py`) first looked like 349 lots, but that
used the RAW flag. The shipped filter applies the **runtime** flag semantics: `resolve_loot_flag`
returns 0 for repeatable/temp flags (`== -1` or `>= 0x40000000` — golden runes, gloveworts, crafting
that re-drops), which is exactly the notability cut. With that applied (`sim_enemy_pass.py`, mirrors
`build_disk_enemy_markers` over the disk `Parts.Enemies` parse):

| filter (runtime flag semantics) | markers | covers baked-119 (by lot) | extra notable |
|---|---|---|---|
| `persistent flag` only, **dedup by lot** (SHIPPED) | **~124** | 81 | +43 |
| `persistent flag` AND `placements == 1` | ~101 | 81 | +20 |

- **`getItemFlagId`** (on the `ItemLotParam_enemy/_map` row) is the game's persistent "you took this"
  flag. Respawning clutter has flag 0; one-time named drops have a real flag. **All 119 baked enemy
  lots carry a persistent flag**; only ~5.8% of universe placements do.
- The +43 extra are notable drops the bake's manual curation MISSED — Blaidd's & Ronin's armour sets,
  Hoslow's Petal Whip, Patches'/Miriel's Bell Bearing, Inseparable Sword + Twinned set, etc.
- **Coverage caveat (81/119 by lot, not 119):** 35 baked enemy lots use a *sibling* `ItemLotParam`
  row — the npc's `itemLotId` is the base lot, the bake resolved the item-bearing row in the same lot
  sequence (e.g. base `333062020` → item `333062021`). The disk pass resolves the base, so it does NOT
  lot-match those 35 (they stay baked; the measured duplicate overlap with disk markers is only ~3).
  Exact 119-match would need RE of the `ItemLotParam` row chain — marginal. 3 more (m61 DLC: Blessed
  Bone Shard ×2, **Iris of Occultation**) have a one-time flag in a high range `resolve_loot_flag`
  treats as repeatable, so the disk pass skips them and the bake keeps them — correct (no loss).

The notability signal is runtime-readable (`resolve_loot_flag(lot)` already exists), so
`build_disk_enemy_markers` filters live — no `LOOT_CATEGORIES` port. SHIPPED in the pass (config
`loot_enemy_drops`): ~124 notable enemy markers, drops the lot-matched baked Enemy rows, adds the ~43
the bake missed; the lot-exact provenance guard keeps the sibling-row + DLC-flag baked rows.

Full 119-row table: see [`enemy_markers_table.md`](enemy_markers_table.md) (this dir).

---

## 3. Treasure split — `partBucket='live'` (disk-reproducible) vs NONE (residual)

Joining the 3639 baked Treasure rows to `items_database.json` by `itemLotId`:

| partBucket | rows | meaning |
|---|---|---|
| **live** | 3325 | bound to a LIVE `Parts.Asset` via `Events.Treasure` → the disk pass reproduces it |
| **NONE** | 312 | corpse/body item-pickup loot bound by an OFFLINE heuristic → NOT cleanly disk-reproducible |
| reachable_dummy | 2 | DummyAsset w/ EntityID (4910/15000990) → recovered via the +0x60 entity offset |

(The runtime `[DEBAKE-GAP]` counts **328** uncovered Treasure rows — same root, slightly different
denominator: the offline join keys on the first DB row's `partBucket`, the runtime counts lots the
disk pass did not place. Both point at the same ≈312–328 corpse-loot residual.)

**Where the NONE residual is** (map of the 312): **m60 204 (65 %)**, m12 20, m11 16, m61 15, m31 10,
m15 10, m18 8, others. Matches the "67 % m60" earlier estimate.

**Root cause** (confirmed, see [[aeg-collectible-source]] DEBAKE-RECOVER): these are AEG099_630/090
**corpse pickups** (Fire Monk + Cuckoo Knight armour sets, Crimson Hood) delivered via
`pickUpItemLotParamId` on models OUTSIDE the `_8xx` collectible scope. Their pickup lot ≠ the
corpse's armour lot, so widening the collectible filter recovers **0**. They are bound to their
asset by an offline heuristic (`enrich_fallback_with_emevd.py`), not a clean MSB structure the
parser reads → **they must STAY BAKED as a curated residual (~9 % of the treasure slice).**

---

## 4. The Unknown 4366 — overwhelmingly NON-loot map features (out of scope)

These are not loot rows; they are curated map features (4337 of 4366 have no lotId):

| category | rows | nature |
|---|---|---|
| LootMaterialNodes | 1455 | GEOM-tracked gather-node positions (collectible overlay covers identity live) |
| ReforgedRunePieces | 1227 | GEOM-tracked piece locations (baked pos + live flag) |
| ReforgedEmberPieces | 298 | ″ |
| WorldStakesOfMarika | 439 | revival stakes |
| WorldQuestNPC | 344 | quest NPC pins |
| WorldSummoningPools | 245 | summoning pools |
| WorldInteractables / SpiritSprings / HostileNPC / ImpStatues / Maps / Paintings / … | ~360 | misc world features |

The 2980 piece/node rows (MaterialNodes + Rune/EmberPieces) overlap the shipped
`loot_collectibles` runtime feature (which overlays live identity) but keep their baked GEOM
position as the tracking authority. The rest are genuinely curated map furniture — **never part of
the loot no-bake goal.**

---

## 5. THE NO-BAKE MATRIX — what really keeps the bake alive

### LOOT slice (the de-bake target)

| slice | rows | no-bakeable? | mechanism | status |
|---|---|---|---|---|
| Treasure `live` | 3325 | ✅ YES | disk `Events.Treasure` pass | **SHIPPED** (`loot_from_disk_msb`) |
| Treasure reachable_dummy | 2 | ✅ YES | +0x60 EntityID recovery | **SHIPPED** |
| Treasure NONE (corpse) | ~312–328 | ❌ NO | offline corpse heuristic, no clean MSB structure | **STAYS BAKED** (curated residual) |
| Enemy | 119 | 🔶 YES *if built* | `Parts.Enemies`+`NpcParam` pass (offline-proven) | **NOT YET BUILT** |
| Emevd | 529 | ❌ mostly NO | scripted grants, no MSB world pos (#2 CLOSED: ~unplaceable) | **STAYS BAKED** |
| Collectibles | (runtime) | ✅ YES | AEG_8xx `pickUpItemLotParamId` pass | **SHIPPED** (`loot_collectibles`) |

### NON-loot features (inherently baked — not a de-bake target)

| slice | rows | note |
|---|---|---|
| GEOM pieces/nodes (Unknown) | 2980 | baked position authority; runtime overlays identity |
| World features (Unknown) | ~1386 | graces already live; stakes/NPCs/pools/maps/etc. curated |

### What truly forces the bake to live, within LOOT:

```
Emevd grants            529   (unplaceable scripted grants)
Treasure corpse residual ~312–328 (offline heuristic, ~9% of treasure slice)
---------------------------------
genuine baked-loot floor ≈ 840–860 rows
+ Enemy 119  → removable IF the Parts.Enemies pass is built (else +119 = ~960–980)
```

Everything else in the LOOT slice (Treasure live 3325 + reachable_dummy 2 + collectibles) is
already runtime-derivable today.

---

## 6. Verdict & next step

**A quasi-complete no-bake of the LOOT tranche is reachable.** After building the enemy
`Parts.Enemies`+`NpcParam` runtime pass, the only baked-loot residue is:
- **Emevd 529** (scripted, genuinely no world position), and
- **~312–328 corpse-loot Treasures** (offline heuristic).

≈ **840–860 explicitly-curated baked loot rows** (~10 % of the original loot universe), everything
else live. Encode the boundary in `loot_source` (Treasure `live` → disk-replaceable; Treasure NONE
→ CuratedResidual that stays baked; Emevd → stays baked; Enemy → disk-replaceable once the pass
ships).

**Highest-leverage next:** build the enemy disk pass — `msbe_parser` parse `Parts.Enemies`
(name/pos/NPCParamID), `loot_disk` join live `NpcParam.itemLotId_enemy`/`_map` → `ItemLotParam_enemy`,
emit at the Part position, add the lots to `disk_lots` so the provenance guard drops the 119 baked
Enemy rows. Offline oracle = `extract_all_items.py:533-567` (must match the 119 exactly).
The non-loot Unknown features + the two residuals stay baked by design.
