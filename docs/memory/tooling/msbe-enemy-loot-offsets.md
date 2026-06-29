---
name: msbe-enemy-loot-offsets
description: "Pinned offsets for the no-bake ENEMY-drop pass: MSB Enemy part type/NPCParamID + live NpcParam itemLotId fields"
metadata: 
  node_type: memory
  type: project
---

**Enemy loot is no-bakeable via `MSB Parts.Enemies → NPCParamID → NpcParam.itemLotId → ItemLotParam`,
parallel to the treasure/collectible passes.** Offsets pinned 2026-06-25 (probes
`tools/probe_enemy_npc_offset.py` + `tools/probe_npc_lot_offset.py`, validated over all ERR _00 MSBs +
the NpcParam paramdef). For the disk-loot enemy pass on branch feat/msbe-entity-recover-dummy.

**MSB Enemy PART entry (same section walk as msbe_parser, SEC_PARTS=5):**
- **part-type @ +0x0c == 2** = Enemy (histogram over ERR: type 2 ×26310, type 10 DummyEnemy ×20, type 4
  Player ×4). The offline `msb.Parts.Enemies` = type 2 only.
- **NPCParamID = `*(u32)( *(u64)(part+0x68) + 0x0c )`** — match **26277/26277** vs SoulsFormats
  `p.NPCParamID`. `part+0x68` = the Enemy typeData pointer (entry-relative on disk / abs VA resident,
  same `eio()` rule as the treasure path). Distinct from `part+0x60` = the entity sub-struct the
  treasure path reads (EntityID@+0x00). (`*(part+0x60)+0x4c` reaches the same field redundantly because
  typeData = entity_struct+0x40.)
- **position = part+0x20** (X@+0x20, Y@+0x24, Z@+0x28) — uniform part-header layout, same as Asset/Treasure.

**Live NpcParam (regulation, get_param<RawNpcRow{736}>):**
- **DetectedSize = 736** (NPC_PARAM_ST).
- **itemLotId_enemy = s32 @ +0x30** (→ resolve with lotType 2, ItemLotParam_enemy).
- **itemLotId_map   = s32 @ +0x34** (→ resolve with lotType 1, ItemLotParam_map).
- both = -1 (0xffffffff) when unset. Adjacent: getSoul@+0x2c, itemLotId_enemy@+0x30, itemLotId_map@+0x34.
- Offsets computed from the applied paramdef with SF bit-packing (u8+dummy8 bitfields share a unit,
  grouped by bit-LIMIT not DefType name); row total == 736 confirms.

**Offline derivation to MATCH (extract_all_items.py:533-570):** iterate Parts.Enemies; skip
`GameEditionDisable==1`; `lot = NpcParam.itemLotId_map` (PREFERRED, lotSource 'map') else
`itemLotId_enemy` (lotSource 'enemy'); skip `is_spawn_broken` (ERR m30_08 c4020_9000); source='enemy',
partName = enemy part name, pos = part pos. The 119 baked `LootSource::Enemy` rows (all lotType 2 →
all came from itemLotId_enemy) are the curated subset; the runtime pass adds lots to disk_lots and the
provenance guard drops the baked Enemy rows it covers (an enemy DEBAKE-GAP = baked-Enemy not covered).
GameEditionDisable offset on the part NOT yet pinned — measure over-emission first.

**★ ENEMY DE-BAKE GAP DIAGNOSED 2026-06-25 (branch diag/loot-residual-source): 84/119 covered, 35 residual =
non-`_00`-tile enemies.** The runtime [ENEMY-MARKERS] split (parsed_enemy_lots = every parsed enemy's raw 0x30
lot via `npc_item_lot_enemy`) showed **0 parsed-but-uncovered + 35 not-parsed** → NOT the map-preference bug
(npc_loot_lot prefers 0x34), NOT a filter. ROOT CAUSE: the OFFLINE bake globs **ALL** MSB LOD tiers
(`MSB_DIR.glob('*.msb.dcx')`, extract_all_items.py:385) + skips GameEditionDisable==1, and rewrites every pos
to `_00` (:257); the RUNTIME loot/enemy pass parses **`_00` ONLY** (load_disk_treasures, 651/964 tiles, the
313 non-`_00` skipped as LOD-proxy/GED-dupes). So the 35 are enemies present ONLY in non-`_00` tiles (GED
variants `_10/_11/_12` / LOD `_01/_02`) → their npcParamId is in NONE of the 26057 `_00` enemies → never
covered. They map to Gloveworts ×11, Golden Runes ×6/Low ×3, Talismans, Armaments, Armour, Larval Tears…, by area
m60=15 m61=3 + dungeons m11/m12/m15/m20/m30/m31/m35/m39=17.

**★★★ VERDICT 2026-06-25 — the 35 are STALE committed-bake artifacts, NOT recoverable loot and NOT a runtime
bug. The live runtime is MORE correct than the bake by omitting them.** Investigation arc (each hypothesis
killed by the next measurement): (1) "non-`_00` scope" — WRONG, parsing the 293 non-`_00` tiles recovered 0/35.
(2) "moved to itemLotId_map(0x34)" — WRONG, 0/35 at 0x34 either. (3) full NpcParam-table scan (every row, placed
or not; 1167 distinct 0x30 + 60 0x34): **0/35 at NEITHER field**. (4) **DECISIVE — paramdef-authoritative offline
scan (tools/_probe_enemy_residual.py, reads NpcParam by FIELD NAME, independent of any hardcoded offset): EXACT
same numbers as the runtime (6868 rows, 1167/60 distinct, 0/35 referenced) → the runtime 0x30/0x34 are CORRECT,
offset BLANCHI; bake-source (<windows_downloads>\ERR_mod) and deployed regulations are IDENTICAL (no current drift);
32/35 exist as ORPHAN ItemLotParam_enemy rows (no NpcParam points to them = dead loot), 3 exist nowhere.** So the
committed goblin_map_data emitted these 35 as LootSource::Enemy from an OLDER ERR regulation where some NpcParam
referenced them; the current regulation has no enemy dropping them → phantom/stale markers. **A BAKE REGEN would
drop them.** <user>'s "offset bug" instinct found a REAL bug (bake staleness), just not the runtime offset he
suspected. Reclassify in the scoreboard as "stale-bake", NOT "enemy de-bake gap". ⚠ The non-`_00` enemy parse +
GED-disable pin is UNNECESSARY — do not build it. Spent measurement scaffolding (load_disk_enemies_nonlod,
npc_item_lot_map, npc_all_lot_sets, [ENEMY-NONLOD], diag_enemy_all_tiers, _probe_enemy_residual.py) can be
reverted; KEEP the durable triage: `npc_item_lot_enemy` + [ENEMY-MARKERS] parsed-vs-not split + [RESIDUAL-SRC]
per-category×source (build_buckets_impl, gated lootFromDiskMsb). **★★★★ VANILLA TEST (2026-06-25, _probe_enemy_residual.py extended to GAME_DIR regulation) REFUTED the
"ERR removed vanilla enemies" theory: VANILLA NpcParam ALSO references 0/35 (7039 rows, 1376 distinct
itemLotId_enemy, 0/35).** So these are NOT NpcParam enemy drops in ANY version (vanilla or ERR). Yet mapgenie
shows them as real in-game loot — described as "dropped on a sorcerer BODY", "knight", "Bloodfiends boss",
"albinauric wolf rider". A "body"/corpse you loot = an MSB Treasure (ground item on a corpse), NOT a live-enemy
NpcParam drop. So the bake MIS-LABELED these 35 as LootSource::Enemy when they're really CORPSE/EMEVD-scripted
loot (ItemLotParam_map/Treasure or scripted). Tell-tale: the 3 small lots (113601 Academy Glintstone Staff,
111001, 104512) don't even exist as ItemLotParam_enemy rows → plainly bad bake data. **FINAL VERDICT: the
NpcParam enemy pass is COMPLETE+CORRECT (84/119); the 35 are mislabeled non-NpcParam loot — the real items are
covered elsewhere by the treasure/collectible/emevd passes (or are phantom dupes). Accept the 35.** The vanilla
0/35 also KILLS the "keep load_disk_enemies_nonlod for Vanilla" rationale → REVERT the whole [ENEMY-NONLOD]
scaffolding (load_disk_enemies_nonlod, npc_item_lot_map, npc_all_lot_sets, diag_enemy_all_tiers, the measure
block, parsed_enemy_lots_map, uncovered_enemy_lots); KEEP only npc_item_lot_enemy + [ENEMY-MARKERS] split +
[RESIDUAL-SRC]. _probe_enemy_residual.py kept as the reusable orphan-lot/name-resolve diagnostic.
**★★★★★ CORRECTION 2026-06-26 (tools/_probe_item_sources.py): the "35 enemy = droppable phantoms" verdict was
TOO HASTY. Checking whether each item is granted by ANY OTHER ItemLotParam lot besides its phantom enemy-lot:
SOME are covered elsewhere (Revered Spirit Ash = 25 other map lots; Academy Staff/Witch's Crown/Gaius's Greaves
each have a map-lot → plausibly drawn elsewhere) BUT some are UNIQUE — `Bloodfiend's Arm` (lot 508000701) is
granted by NO other lot in the whole regulation, no NpcParam → it's a SCRIPTED EMEVD BOSS REWARD mis-tagged
Enemy. Dropping it would erase that item from the map. So the 35 SPLIT: ~common/map-lot ones are covered-
elsewhere (safe to drop), but the unique boss/scripted rewards (Bloodfiend's Arm, likely Gaius/Prelate too) are
REAL loot RECOVERABLE VIA THE EMEVD PASS, not phantoms.** This MERGES the "enemy" residual into the EMEVD-
scripted recoverable lever (15 emevd + the unique subset of the 35 + 29 unknown chests = real ItemLotParam_map
loot no disk pass reproduces). NEXT: trace lot 508000701 + the 15 emevd lots + a chest (31080700 m31) in the
mod's event/*.emevd + MSBs to find the placement mechanism, then extend kEmevdTemplates (boss/direct rewards)
and/or add a chest-asset pass. Probes: _probe_residual_family.py (by-family triage), _probe_item_sources.py
(other-source check), _probe_enemy_residual.py (orphan-lot/name).

See [[handoff-loot-from-real-files]], [[aeg-collectible-source]] (collectible pass = same shape),
[[msbe-dummyasset-filter]] (the +0x60 entity offset). Oracle for validation =
docs/re/enemy_markers_table.md (the 119 rows w/ ChrIns + npcParam).
