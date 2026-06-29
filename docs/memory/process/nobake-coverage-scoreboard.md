---
name: nobake-coverage-scoreboard
description: "Marker provenance tracking: Marker::source + [COVERAGE] runtime log + docs/nobake_scoreboard.md (goal = zero baked). How to read regressions/progress."
metadata: 
  node_type: memory
  type: project
---

**★ MERGED-TO-MASTER 2026-06-27 — baked 16 → 1 (NOT 0 — see the correction below)**, merge 04d3758
(branch feat/enemy-sibling-walk) + revert b77e8b5 + census 64773c8. NOT yet pushed.
1. **Enemy sequence-sibling walk** ✅ CORRECT (the enemy pass never had the Mechanism-C walk the
   treasure+emevd passes did → shared `emit_lot_siblings`). Recovered the base+k boss/enemy bundle drops
   wrongly filed irreducible: Larval Tears 2→0, Armour/Ashes/Seeds/Reusables/Golden Runes. baked 16→1,
   236 siblings. RUNTIME-VALIDATED, sound mechanism. See [[disk-parser-coverage-gaps]].
2. **EMEVD 2009:00 "asset-lot" pass ❌ WRONG — REVERTED (b77e8b5).** I read `2009[00]` as place-lot-at-
   asset; the **EMEDF (tools/er-common.emedf.json) proves it is "Register Ladder"** (args = DisableTop
   flag, DisableBottom flag, Entity). The "lots" were ladder disable-FLAGS that only coincidentally equal
   ItemLotParam row numbers (FromSoft reuses the m35/m12 id range). It placed 4 PHANTOM markers at ladders
   (Golden Rune 35000580 + 3 Siofra goods). Caught by building the reachability census + EMEDF typing.
**HONEST STATE: baked = 1.** The lone residual (Golden Rune 35000580 @m35) is an UNPLACED ItemLotParam
row the bake mis-positioned at the ladder = a BAKE PHANTOM, not a real missing marker. The no-bake endgame
for it is to DROP it, not place it (no generic unplaced-lot drop exists yet). **LESSON: before building an
EMEVD loot pass, confirm the instruction's semantics with the EMEDF (arg TYPED "Item Lot"), never infer
from arg co-occurrence — a lot-number can be a flag/entity coincidence.** [[disk-parser-coverage-gaps]]
**GUARD: tools/lot_reachability_census.py → docs/lot_reachability.md** = the independent "am I missing a
pass?" oracle (notable universe reached/unreached + EMEDF-precise missing-pass detector). Current: 0
confirmed missing passes; the only position-bearing item instr (2003:34 Spawn Snuggly) is unused.

The mod tracks **per-marker provenance** to drive the no-bake migration (goal: every map
marker comes from live mod files / game memory, **zero from the static `goblin_map_data`
bake**). Shipped 2026-06-25 (commits e98d723 + 0f9ccd8).

**Mechanism:** `worldmap::Marker` has `source ∈ {Baked, DiskMSB, Live}` + `bool live_classified`
(category resolved via the live `classify_item_live` fallback = item the baked table didn't
know). Stamped at the single chokepoint `push_marker` (every category except graces funnels
through it; `map_entry_layer.cpp`) + `GraceLayer` (Live). Sources: MAP_ENTRIES→Baked (default),
`build_live_bosses`→Live, the 4 disk passes (loot/collectible/enemy/emevd)→DiskMSB, graces→Live.

**Runtime output:** `build_buckets_impl` logs a `[COVERAGE]` scoreboard once per map build —
per category `baked/disk/live/live-cls/total` + status, sorted baked-heavy first.

**Versioned doc + regression loop:** `tools/nobake_scoreboard.py` parses the latest `[COVERAGE]`
block → `docs/nobake_scoreboard.md` (alphabetical = stable diffs, headlined by "baked remaining").
After a change: run game + open map → `py -3.14 tools/nobake_scoreboard.py` → `git diff` the doc.
baked↓ = progress (migrated to disk), baked↑ = regression, 🟡→🟢 = category finished.

**Branch state (2026-06-25):** master is now SYNCED with origin/master (both at merge 6dfb3fb after the
geom-dedup feature) — the prior "93 ahead, unpushed" gap is closed (a push happened; <user> has a push
hook). Workflow now: each feature branches off master, runtime-verified → `--no-ff` merge to master →
delete the branch (flat topology, see [[workflow-preferences]]). Remote cleanup of old merged origin/*
branches may still be pending; KEEP `origin/main` (someone else's work) + any unmerged tooling branch.

**✅ AMMO ROUTING FIXED + MERGED 2026-06-25 (master b9db411): Loot - Ammo 🔴→🟡 (disk 0→78),
Equipment - Armaments de-inflated (disk 356→278).** Ammo was NOT a missing source — it was a cat-2
key-encoding MISMATCH. The disk loot pass already parsed + positioned the ~78 ammo treasures, but
mis-bucketed them into Armaments. ROOT CAUSE: `encode_live_item` (goblin_inject.cpp) keys ALL cat-2
(weapon+ammo) at **+100M** (ammo names live in WeaponName.fmg), but the generator's icon table
(`generate_loot_massedit.py:1075 _encode_item`) keyed ammo (ids≥50M) at **RAW 50M** → at runtime
`item_marker_category(150M)` missed the raw-50M entry → fell to `classify_item_live(150M)` → EquipArmaments
(no ammo sub-case). TWO complementary fixes: (1) RUNTIME (immediate, no regen): `classify_item_live` now
routes a +100M weapon key with raw id≥50M → LootAmmo (ammo shows as live-cls); (2) GENERATOR root cause:
_encode_item keys cat-2 ammo +100M (goblin_item_icons.cpp NOT regenerated — runtime fallback owns ammo
until a full pipeline run). No false positives: only ammo has id≥50M in cat-2 (weapons <50M). baked
unchanged (995) — a categorization fix, not a bake-count drop; the 78 ammo were already disk-dropped from
the bake, just in the wrong bucket. 3 baked LootAmmo residual (2 src=Emevd lots 20650/20810 + 1 uncovered
treasure) = generic debake-gap, not chased. **LESSON: a 🔴 BAKED-ONLY loot category with disk=0 but
many baked lotType-1 rows = suspect the live key-encoding (encode_live_item vs the icon table), not a
missing disk source — the disk pass may be placing them in the WRONG category.** See [[re-offset-validation]].

**NEXT STEPS toward zero-bake (priority order):**
1. **✅ DONE −546 baked — MERGED to master 2026-06-25 (merge 6dfb3fb; on origin/master too).** Final design =
   a **generic position dedup at finalize** (<user>'s idea — robust to future double-counts of this kind,
   vs the first cut which was a per-model `is_collectible_8xx_node` guard). Mechanism in `build_buckets_impl`
   (src/worldmap/map_entry_layer.cpp): the disk collectible pass records each gather-asset's projected cell
   (`Cell{group, lround(worldX*2), lround(worldZ*2)}` = 0.5-unit grid) into `collectible_cells`; after
   build_live_bosses(), any **Baked** marker in the 3 lotId=0 geom categories (Material/Rune/Ember Pieces) that
   lands on one of those cells is erased. **KEYED TO THE COLLECTIBLE PASS ONLY** — an earlier all-disk-marker
   key falsely dropped 11 Rune Pieces (a treasure coincided within 0.5u); collectible-scoping fixed it. Chose
   runtime (not editing generate_material_nodes.py) to avoid a full ERR regen (wipes goblin_location_alt.cpp,
   see [[aeg-collectible-source]]) + keep baked _8xx as off-collectibles fallback. **✅ RUNTIME-VALIDATED**
   (build 09:32:52): `[LOOTDISK] finalize dedup: dropped 546`, Material Nodes 1455→909, Rune 1227 + Ember 298
   UNCHANGED, TOTAL baked 4744→**4198**. Log line to watch on regressions: `[LOOTDISK] finalize dedup: dropped
   N`. Material Nodes still 🔴 BAKED-ONLY — the residual 909 (_6xx/_7xx/_9xx) have no disk source by design.
2. **✅ DONE — Rune/Ember Pieces migrated (merge 57b977a, 2026-06-25).** Built the **runtime GEOM-tracking
   infra** (`goblin::collected::register_runtime_entries` + RuntimeEntry; worker stages, refresh() drains on
   its thread → no race) so disk markers gray like baked pieces. Un-skipped AEG099_821/822 in the collectible
   pass → placed from real MSBs into Reforged categories (identity = the OBJECT, ActionButtonText-confirmed
   "Collect rune/ember piece"; the Trace lot is only a shadow reward — see [[aeg-collectible-source]]). Dedup by
   **IDENTITY (tile, part name)** not position (the m11_10 Leyndell Ashen Capital bake stored an offset pos
   raw+(-2195,-352) → positional dedup missed 12). Runtime-validated: Rune 1227→114, Ember 298→19 (boss-flag
   pieces stay baked, no AEG twin); TOTAL baked 4198→**2806**. DiskCollectible now carries the MSB part name +
   posY. ⚠️ VISUAL graying confirmation in-world CONFIRMED 2026-06-25
   (<user>: "rune trouvé grisé"). The `register_runtime_entries` infra is REUSABLE for any future geom family.
3. **World features (Stakes 439, Quest NPC 344, Summoning 245, Spirit Springs, Imp, Maps, Paintings…):**
   no disk loot source + **no object_name in the bake** (verified) → need per-feature RE (which MSB Asset
   model / Region = each feature; resident MSBE parser, [[runtime-msb-resident-plan]]). Biggest block left (~1300).
   **★ KEY 2026-06-25: the per-feature "which MSB model" RE is ALREADY DONE** — encoded in the
   `tools/generate_*.py` the bake came from. Full identity table + difficulty tiers + the port pattern =
   [[world-feature-msb-identities]]. **✅ Stakes of Marika MIGRATED + RUNTIME-VALIDATED 2026-06-25**
   (branch feat/world-features-stakes, config `world_features_from_disk`): AEG099_060 asset →
   `build_disk_stakes_markers` (rides the collectible asset enumeration). **Finalize = CATEGORY-WIPE**
   (drop ALL baked Stakes when the disk pass placed ≥1, like live bosses) — NOT a positional dedup. WHY:
   the bake is an unreliable oracle here — `generate_stakes.py` scans EVERY tile incl. LOD-coarse `_02`
   (proxy objs at 128/256 offset; parse `_00` only) AND its dedup key omits gx,gz → baked rows inflated
   with offset LOD-phantoms (sonde: 226 AEG099_060 in `_00` vs 250 in non-`_00`; bake=439 but disk
   world-cells=219). Positional dedup left ~224 phantoms drawn at wrong spots; category-wipe is safe (all
   651 `_00` MSBs parse, ground stake always in a `_00` tile). Result: `[COVERAGE] Stakes baked=0 disk=219`
   🟢 off-bake; `dropped 439 baked (category-wipe)`; baked total 1908→1469. NEXT easy ones (pure Asset
   scan, same pattern, same finalize): Imp/Seal AEG027_078/079, Hero Tomb AEG099_055 — but VERIFY each
   feature's bake isn't similarly LOD-phantom-inflated (the `_00`-only world-cell count is the truth).
4. Small loot (Gestures 7, Great Runes 6, Ammo 3) — check why the disk passes miss them. + Reforged boss
   pieces (Rune 114 / Ember 19, EMEVD event 1200) could go via the emevd disk pass.
5. **✅ DONE + RUNTIME-VALIDATED (branch feat/scoreboard-census-flag-coverage, 2026-06-25) — scoreboard
   tooling upgrade.** Per-category `[COVERAGE-CENSUS]` runtime line in `build_buckets_impl` (next to
   `[COVERAGE]`): `drawn` (real markers = baked+disk+live) · `census` (ImGui badge denominator =
   completable spots, computed EXACTLY like `refresh_overlay_census`: distinct collect/cleared flags for
   flag-based cats, row count for geom/SFX pieces+kindling, 0 for graces) · `flagged`/`respawn`/`nonloot`
   (collect-flag coverage). `tools/nobake_scoreboard.py` parses it (merged by category) → "Census (badge
   vs drawn) + collect-flag coverage" table; doc regen committed (commit 2d24614). VALIDATED insights it
   surfaced: Stakes drawn=219 census=0 flag=0/219 (respawn pts, no collect flag); Summoning Pools
   census=245 flag=245/245 (fully flagged); Armour drawn=246 census=119 (markers share flags, badge dedups).
6. **✅ Gather "flood" RESOLVED (was a bug, not curation) — merge 0878ee4.** The ~16800 was the wrong-bit
   leak (read isBreakOnPickUp not isEnableRepick), NOT the gather universe. Fixed → ~1464 real gather ≈ the
   bake's 1455. No per-node curation needed. (Earlier "PARKED/pre-existing" framing was wrong — see the
   Material Nodes note below.)

**★★ REFORGED RUNE PIECES 9→0 — bossEntity@16 recovery (MERGED to master, merge 3f3d66e, 2026-06-26, awaiting <user> push).**
The 9 baked Reforged Rune Pieces were the LAST clean loot lever. ROOT CAUSE: a runtime↔bake DISCREPANCY in
the boss-reward (90005860/61/80) parse. The OFFLINE bake reads the EXPLICIT bossEntity@16 arg
(extract_all_items.py:651 entity@16,lot@24) and positions the piece there; the RUNTIME (msbe_parser.cpp:653)
parsed ONLY (defeatFlag@8, baseLot@24) and joined defeatFlag→boss as an entity (Reforged convention
defeatFlag==EntityID, ~83/92 binds) with a fragile setter-candidate fallback. For 9 overworld field bosses
(c4980/c3150/c4950 + 1 cross-tile c4503) defeatFlag != EntityID → join failed → baked. FIX = parse
bossEntity@16 (BossFlagLot struct {flag,lot,entity}) + use it as a position fallback AFTER flag, BEFORE the
setter (gray flag still = piece lot's getItemFlagId, so @16 is purely position; flag==entity@16 for all 83
"both" binds so flag-first never shifts a validated marker). 8/9 entities resolve in _00; the 1 cross-tile
(c4503 entity 1054560800 in m60_13_14_02 LOD, bind in m60_54_56) recovered by extending missing_award_entities
to bossReward awards (the existing load_lod_award_entities LOD scan). De-bake via the tile-scoped __BOSSRUNE__
piece key (same path that already dropped 1516). OFFLINE-PROVEN: tools/_probe_boss_piece_entity.py = 83 both /
9 flag-only / **9 ENTITY-only (the residual)** / 0 neither; 9/9 baseLot chains hold a real Rune piece (base+1/+2)
w/ gray flag. **✅ RUNTIME-VALIDATED 2026-06-26 (build 12:37): `9 boss-via-entity@16`, `0 boss-no-entity`, boss-reward
pieces emitted 140→149, `replaced 1525` (was 1516), `Reforged - Rune Pieces baked=0 disk=1244` 🟢, `LOD award-entity
scan 17 resolved` (incl. cross-tile c4503), TOTAL baked 37→28, ZERO other category changed.** LESSON: when a baked residual has a
clean bake-side source the runtime parser DOESN'T read (here: an explicit arg offset the runtime skipped for a
convention shortcut), it's category-A disk-parse-gap — diff the bake's extractor vs the runtime parser arg-by-arg.
The remaining 28 = 11 Golden Runes (irreducible, don't chase) + World Interactables 2 + Maps 1 + 14 misc loot
(re-probed by name 2026-06-26: real named items — Spirit Ashes/Scarseal/Larval Tears/orphan-enemy GR — the
already-triaged ACCEPT class, no new lever; orphan ItemLotParam_enemy w/o NpcParam/EMEVD + per-tile chest templates).

**★★ [BAKED-RESIDUAL] BULK DIAG = THE authoritative residual feed (commit d7710cb, 2026-06-27).** The offline
goblin_map_data/items_database parse is ERROR-PRONE (doesn't see runtime recategorize/finalize drops; a row's static
textId1 ≠ its live-resolved item — it mis-identified BOTH the Maps + Interactables residuals). FIX: dump the truth from
the runtime. `[BAKED-RESIDUAL]` (gated diag_loot_pos) scans g_buckets AFTER all passes + the world-feature finalize →
one self-describing line per surviving `source==Baked` marker across EVERY category (the old `[RESIDUAL-ROW]` was
loot-only, lotId-keyed, pre-finalize — why World features were invisible to it), with the item NAME resolved LIVE
(goblin::lookup_text_utf8 of the marker's key, localized) → zero offline step to identify a residual. Per-category count
+ TOTAL. Marker now carries lotId/lotType (set in push_marker). VALIDATED build 00:29: all 16 dumped w/ names, TOTAL=16.
**RULE: to identify/triage what's still baked, read [BAKED-RESIDUAL] from the log — do NOT grep goblin_map_data offline.**
The 16 confirmed = 15 lt=2 orphan-enemy (no NpcParam) + 1 lt=1 m35 (0,0,0) fallback = positionless-irreducible.

**★★ COVERAGE DIAGNOSTIC TRIO (#1-#3) — all runtime-fed + versioned in docs/nobake_scoreboard.md (commits 629194a +
69f8d09 + cab684a, 2026-06-27; on master).** Three additions, all driven by "offline parsing is error-prone, read the
runtime":
- **#1 coverage_vs_mapgenie.py REBASED on runtime [COVERAGE]** (was counting the STALE bake). → docs/coverage_vs_mapgenie.md
  + a summary in the scoreboard: **31 categories NOT WIRED** (MapGenie types the mod parses ZERO of: Martyr Effigy 212,
  Elite Enemy 184, Landmark 172, Character/Ghost/Merchant NPCs, Dungeon 64, Scarabs, Divine Tower, Evergaol, Portal,
  Remembrance, Tool, annotations…) + 17 DRIFT (partly ERR≠vanilla). This is the "what whole categories are still missing"
  list — the next FEATURE backlog beyond loot.
- **#2 [SKIPPED] runtime block** (g_skip accumulator, gated diag_loot_pos): disk placements parsed but NOT drawn, by
  reason — unclassified + no_anchor = ⚠️ real coverage gaps (883 + 8809); dedup/by_design(~499k, clutter-dominated)/
  merchant_phantom/dummy_inert = ✅ correct skips. The inverse of [COVERAGE].
- **#3 collect-flag SOURCE split**: [COVERAGE-CENSUS] gains flag-live/flag-disk/flag-baked. **flag-baked == the baked-loot
  total (16)** — a built-in invariant: it IS the un-migrated residual, falls to 0 with the baked total at Phase-2 bake
  deletion. Provenance: 469 live · 4966 disk · 16 baked. Marker gained lotId/lotType.
nobake_scoreboard.py parses all three (CENSUS regex flag-src optional for old logs; imports coverage_vs_mapgenie for the
gap section). Full diagnostic chain now: [COVERAGE] + [COVERAGE-CENSUS]+flag-src + [SKIPPED] + [BAKED-RESIDUAL], all
runtime-fed, all versioned.

**★ MERGE STATE 2026-06-27: feat/nobake-emevd-loose-recover MERGED to master (--no-ff, merge 797427f) + the 4 coverage
commits (629194a/69f8d09/629.../cab684a) on master; <user> pushed (origin/master=797427f confirmed). Branch deleted.
baked 27→16 campaign COMPLETE + on master. ALL clean loot+map+world-feature recovery levers EXHAUSTED.**

**★★ ASSET-AWARE + LOOSE-ANCHOR EMEVD JOIN — recovers 6/7 EMEVD-LOOSE residuals, baked 27→21 (branch
feat/nobake-emevd-loose-recover, commits bc5cf09 + ff347a7, 2026-06-26; ✅ RUNTIME-VALIDATED build 23:30,
UNMERGED — <user> merges --no-ff).** In-game: `[COVERAGE] TOTAL baked=21`, ZERO category regression; Spirits 2→0 /
Talismans 2→0 / Smithing Stones 1→0 flip 🟢, Golden Runes (Low) 5→4; `[LOOTDISK] emevd loose-recovery: 326 via
asset-anchor, 34 via loose-anchor, 9 boss-base; 5278 MSB assets indexed`; the ~90 asset/loose recoveries landed as
REAL coverage (Armour disk 243→327, Spirits 75→86, Talismans 136→145), 🟢 off-bake 49→52 of 61. Scoreboard regen'd.
Root cause of the LOOSE residual: the EMEVD direct-award join (build_disk_emevd_markers) was ENEMY-ONLY, so every
emevd reward anchored to an MSB ASSET stayed baked. Re-triaged the oracle's 7 RECOVER-EMEVD-LOOSE? + 1 EMEVD-BOUND-ASSET
by arg-layout dump (tools/_probe_emevd_loose_args.py) + blast-radius probe (tools/_probe_loose_blast.py — MEASURE the
new∩notable count BEFORE coding, the memory's over-emit discipline) → 4 mechanisms, all shipped scoped:
- **A asset-join** — feed MSB Asset positions (disk_collectibles, adapted to DiskEnemy) to the direct/boss join.
  Recovers Blaidd (1034500110). +~63 REAL asset-anchored awards the enemy-only join structurally missed (paintings
  90005632, great runes 90005110, NPC/quest rewards 90005750) = a no-bake COVERAGE GAIN, not just residual cleanup.
- **B new templates 90005500/501** (lot@8, asset-anchor@20): Golden Rune[200] m12_05 (12050510) + Somber Scadushard
  m20_01 (20010520). **0 notable collateral** (the lot_row_in_table gate filters the 98 placeholder lot@8 values).
- **C loose-anchor** — a direct-template award whose DOCUMENTED entity is a non-positionable logic id → use the nearest
  positionable arg window (enemy or asset). Recovers Taylew (90005555, asset@idx4≠documented@8) + Viridian (90005300,
  enemy@idx3≠documented@8). +~19 real awards. EmevdAward/DiskEmevd carry `anchors` (the init's entity-range windows,
  nearest-to-lot first); the join tries documented entity, then anchors.
- **D boss-base emit** — a bossReward init (90005860/61/80) that drops a Rune Piece AND a real item: the piece-path
  emitted ONLY the piece + `continue`d, dropping the base. Now falls through to emit the notable base too. Recovers
  Radagon's Scarseal (1042330100, base+0 talisman beside its base+1 Rune Piece). +8 real boss-base drops.
**SCOPE GUARD (critical):** asset-join + loose-anchor apply ONLY to direct-template + boss awards (DiskEmevd::allowAsset
= true for p.direct/boss, FALSE for perTileEnemyAward + ev1200). perTile MUST stay enemy-only — an asset-join there
re-introduces the ~395 asset-entity-chest over-match (the m60-chest revert). Two position maps: ent_enemy (perTile/
ev1200) + ent_any=enemy+asset (direct/boss). **DEFERRED: the 2 Larval Tear per-tile residuals** (1047370100 enemy@8,
1049550700 enemy@16 — heterogeneous offsets; per-tile lot@last rule = 342 lots, the over-emit zone the memory forbids;
recovering 2 markers isn't worth the regression risk). Expected baked 27→21. New [LOOTDISK] emevd loose-recovery log
(asset/loose/boss-base counts). Remaining 21 = 16 IRREDUCIBLE loot (orphan-enemy no-NpcParam incl. 11 Golden Runes /
finite-shop / (0,0,0) fallback — NO world position, will simply vanish when the bake is deleted in Phase 2, no recovery
needed) + World Interactables 2 + Maps 1 + the 2 deferred Larval Tears. **★ REFRAME for Phase 1→2: "baked→0" for the 16
positionless = just delete the bake (Phase 2); only the ~11 POSITIONABLE residuals (now mostly recovered) block deletion.**
Probes (all committed): _probe_residual_recover.py (oracle), _probe_emevd_loose_args.py (arg dump), _probe_loose_blast.py
(blast-radius per mechanism). **NEXT: <user> runtime-test → confirm baked→21, the ~90 new asset/loose markers land at
REAL spots (not phantoms — the asset-anchor for paintings/runes/NPC rewards needs the visual eyeball), zero regression →
merge --no-ff.** See [[residual-irreducible-strategy]], [[nobake-endgame-roadmap]].

**★★ CROSS-TILE LOD TREASURE 28→27 (merge c666c00, 2026-06-26, runtime-validated build 13:09).** Found by the
automated recovery oracle (tools/_probe_residual_recover.py). 18 MSB Treasures map-wide live ONLY in an overworld
LOD supertile (cross-tile part `m{AA}_{BB}_{CC}_00-AEG…`); the _00-only load_disk_treasures misses them, most are
sibling-covered, 1 (Dragonwound Grease 1040540050) sits past a chain gap → residual. New `load_lod_treasures()`
(loot_disk.cpp) + baked-loop re-source (kindling pattern, KEYED ON ∉disk_lots so sibling-covered lots never double).
`LOD treasure scan: 19`, `re-sourced 1`, Loot - Greases baked=0 🟢, TOTAL 28→27, zero regression. See
[[residual-irreducible-strategy]] (oracle + the wrong-guard gotcha). ⚠ 3 LOD scans now run on map-open (award-entity
+ feature-asset + treasure), each re-decompresses the same non-_00 MSBs (~25ms ea) — combinable into one pass if perf matters.

**★ Current: baked 37 (MERGED to master 2026-06-26, merge 523cfeb == origin/master; runtime-validated). Session 96→37 (−61%):
(1) 4 Snow Town statues AEG110_029 LOD-asset off-bake (96→92); (2) 42 merchant phantoms dropped via live ShopLineupParam
(92→50, Armaments/Gloveworts→0); (3) per-tile EMEVD enemy-death award pass (50→37: 12 Golden Runes + 1 gem recovered
baked→disk no-double + ~14 new enemy-drop markers the bake missed). See [[residual-irreducible-strategy]].
★ THE per-tile EMEVD enemy-award MECHANISM (commit 9652363): a bank-2000 init whose callee@(a+4) is a per-tile id ≥1e9
(NOT a kEmevdTemplate) awards a lot at enemy death — entity@X0_4(a+8), lot@idx(n-2)(a+argLen-8). parse_emevd_full →
EmevdParse.perTileEnemyAward → load_emevd_awards feeds as lotType-1 direct; the existing build_disk_emevd_markers
entity→disk-ENEMY join FILTERS asset-entity chests (this is what dodges the 395-chest over-match; blast-radius probe:
generic any-enemy-any-lot=489, enemy@idx2+lot@idx(n-2)=21). Baked-drop guard extended to drop src=Unknown rows (the GR
are baked Unknown). Probes: _probe_goldrune_pos/_path2 (12 rec / 11 irreducible), _probe_emevd_blast/_tight/_precise (21,
0-covered, 12/12). Remaining 37 = Reforged Rune Pieces 9 + 11 irreducible GR (9 orphan-enemy no NpcParam/no-emevd + 2
fallback m12/m35) + World Interactables 2 + Maps 1 + misc orphan. Prior 96 = feat/emevd-treasure-dup-debake: 15 emevd lots
de-baked (treasure_dup 7 + LOD-scope 8) → 118, Kindling Spirits off-baked → 113, extra-puzzle Interactables
(Sellia chalices + Siofra lanterns, 17) off-baked → 96. **🔴 baked-only count = 0**. Prior 133 = feat/world-
features-seals. NOT pushed/merged; <user> pushes.**
Campaign arc: 4744 → 4198 (Material _8xx dedup) → 2806 (Rune/Ember→disk) → 1908 (all gather→disk) →
1469 (Stakes) → ... → 989 (Paintings/Gestures/Great Runes) → 677 (treasure sibling-walk) → 333
(Quest NPC retired) → 322 (Material Nodes identity dedup) → 198 (EMEVD boss-drop pieces off-baked) →
**133** (Seal Puzzles AEG099_090 off-baked). −97%.

**★★ SEAL PUZZLES (AEG099_090) OFF-BAKED 2026-06-25 (branch feat/world-features-seals) — baked 198→133.**
The 88 baked World - Interactables were the biggest single baked chunk; ~65 are AEG099_090 "Examine seal"
objects (the multi-seal fog-door puzzles). **KEY INSIGHT (confirmed by RE): "live fallback" (classify_item_live)
CANNOT source them** — it's an item-key→Category classifier (decodes +Nx100M item ids); a seal has no item id →
returns -1. The recoverable path was the **DiskMSB world-feature asset pass** (build_disk_world_feature_markers),
the SAME mechanism that off-baked Imp seals + Hero's Tomb (shared WorldInteractables). Seals qualify: clean MSB
asset identity (AEG099_090) carrying its EntityID + an EMEVD activation flag. Mechanism (ZERO new pass):
(1) +1 editorial row in tools/world_feature_assets.py (AEG099_090→WorldInteractables, entity_required, CELL-dedup
not category-wipe since shared, flag_rule='seal_emevd'); (2) +1 EMEVD template {90006051, 8, 12, 16} in
msbe_parser kEmevdFlagTemplates (seal entity@+8, flag@+12); (3) new FlagRule::SealEmevd in
build_disk_world_feature_markers joins entityId→90006051 flag and **SELF-GATES** (skip if no binding — AEG099_090
is also placed as decoration; the gate, stronger than entity_required, mirrors the bake which only emitted
template-referenced seals). **RUNTIME-VALIDATED (build 22:36):** `[COVERAGE] Interactables baked=23 disk=79`,
**flagged=102/102** (all seals gray), **661 non-seal AEG099_090 rejected by the self-gate = 0 phantoms**. FMG label
800009503 "Examine seal" stays preloaded (bake's seal rows remain in MAP_ENTRIES; only markers dropped at finalize).
Also hardened: dropped the `default:` from the flag-rule switch → clang-cl -Wswitch now flags a future unhandled
FlagRule at COMPILE time (was: silent no-flag marker at runtime). **The 23 baked residual = the 21 "extra puzzles"**
(Sellia chalices AEG099_047, Siofra lanterns AEG110_029, Snow Town statues AEG237_055 — bespoke non-template events,
[[world-feature-msb-identities]]) + 2 — a 2nd pass with per-event handlers (extract_seal_puzzles.py _EXTRA_PUZZLES)
would finish Interactables to ~0. **LESSON: a World feature with an AEG asset identity + an EMEVD/arithmetic flag =
ONE editorial row + maybe ONE EMEVD template, no new C++ — the world-feature pass is the generic off-bake lever.**

**★★ MATERIAL NODES OFF-BAKED 2026-06-25 (merge e4d6e50) — the 11 "dead-end" were a dedup-mechanism bug,
not real residual.** They survived the finalize POSITIONAL cell-dedup (0.5u) because their baked pos was
offset >0.5u from the live MSB gather-node pos — the SAME near-miss the Rune/Ember pieces hit. Fix = mirror
the pieces' IDENTITY dedup: build_disk_collectible_markers records each emitted gather node's
`piece_key(area,gx,gz,c.name)` into `gather_disk_keys`; the baked loop drops a baked LootMaterialNodes row
whose (tile, object_name) matches (right after the piece dedup). **Runtime: replaced 1455/1455 by identity**
(ALL baked Material Nodes have a disk gather twin → category 100% disk-derived, not just the 11), positional
finalize now drops 0 (kept as a fallback for gather nodes with an empty MSB part name). Material Nodes 🔴→
removed, baked 333→322. LESSON: a small 🔴 baked-only residual on a category that's OTHERWISE fully
disk-covered = suspect the dedup KEY (positional vs identity), not "irreducible content" — same root cause
twice now (pieces, then nodes).

**★★ DEBAKE-GAP SOLVED 2026-06-25 (merge 48d2ef5) — it was NOT an irreducible corpse residual.** The
handoff #6 verdict ("~328 baked Treasure rows must STAY BAKED as a curated corpse residual") was WRONG —
disproved by a runtime cause-sample diag (commit 8f98e59: per-uncovered-row sequence-sibling / cat / area
cuts in build_buckets_impl, gated diag_loot_pos; note Treasure rows carry object_name=nullptr at runtime so
the corpse-asset histogram is impossible — classify by lot adjacency instead). Result: **310/328 were
sequence-siblings of a disk-placed base** = MULTI-LOT treasures (a corpse/chest grants a full armour set as a
contiguous ItemLotParam_map chain base..base+k; Armour dominated 126, all m60 67%). The MSB Treasure carries
ONLY the base itemLotId (@td+0x10) so the parser under-read the bundle. **FIX (commit 5dbd26d): treasure
sequence-sibling walk** — `build_disk_loot_markers` now walks each disk treasure's contiguous _map sub-lots
(base+1..+50), emits each NOTABLE one (flag!=0 + real item, via `goblin::lot_row_in_table`) at the SAME
treasure position, classified live — the SAME walker + design as EMEVD mechanism C ([[handoff-loot-from-real-
files]] LATER#5). Stops at a param gap or the next treasure base (`bases` set); suppresses Rune/Ember by goods
id (800010/850010, GEOM-tracked). Runs with loot_from_disk_msb. **RUNTIME-VALIDATED (build 17:19):
`emitted 3404 + 313 sequence-siblings`, replaced 3310→3623, `[DEBAKE-GAP] 328→16`, baked 989→677.** Lost
Ashes + Smithing(Low) flipped 🟢 off-bake; Armour 129→3, Fortunes 54→3, Reforged Items 29→2; NO category
regressed. **LESSON: the window-16 proxy in the diag (any disk lot within 16 below) over-counts on dense
area-sequential lots — the REAL contiguous walk is precise; trust the runtime `replaced`/`emitted`, not the
proxy.** ⚠ NEXT-SESSION: refresh [[handoff-loot-from-real-files]] (the #6 "265 corpse irreducible" floor is
now ~16) + re-run datamine_enemy_loot for the corpse slice if chasing the last 16.

**★★ QUEST NPC RETIRED 2026-06-25 (merge e87388b) — baked 677→333.** The 344 World - Quest NPC rows
(location-only, no quest-state graying, superseded by the in-overlay Quest Browser) are now runtime-skipped
at the build chokepoint (`if (e.category == WorldQuestNPC) continue;`, like WorldBosses; map_entry_layer.cpp).
Zero regen, reversible. **Investigated revive/off-bake first, all dead ends (don't re-chase):** (1) a disk
MSB pass (hostile-NPC clone) would reproduce POSITIONS but still has no per-NPC completion state; (2) NO
clean quest-state source exists — ER tracks quest progress in ESD state machines + thousands of scattered
event flags, no queryable per-NPC structure (the mod's own Quest Browser is HAND-AUTHORED, its only runtime
signal a manually-captured per-NPC `fail_flag`); (3) **live RPM is not a fast path** — ChrIns/EnemyIns are
resident ONLY for the streamed tiles around the player (vtable-scan probe `<ghidra_scripts>\chr_enum.py`:
461 EnemyIns total, ~108 positioned <300u of the player, 2 far — never the ~344 world-spread NPCs), same
streaming constraint as the loot walker [[fieldins-pool-registry-re]]. So a whole-world map/browser CANNOT be
derived from memory, for positions OR state. **Full pipeline cleanup deferred** (generator generate_quest_npcs.py
/ `show_quest_npc` toggle / icon 443 still present, just unused). Quest Browser (hand-authored NpcQuest table,
goblin_quest_steps.cpp) is the going-forward UX, unchanged. New reusable probe: chr_enum.py (EnemyIns/ChrIns
vtable-scan: ChrIns vt er+0x2a2e0b8, EnemyIns vt er+0x2a44010; WCM=[er+0x3D65F88], player=[WCM+0x1E508]+0x6C0).

**★★★ THE 95-LOOT RESIDUAL FULLY TRIAGED 2026-06-26 (by-family orphan-lot batch probe,
tools/_probe_residual_family.py + the [RESIDUAL-ROW] runtime dump, diag_loot_pos).** <user>'s "check
by family" idea cracked the structure. Each surviving baked loot row classified vs the deployed
regulation (NpcParam 0x30/0x34 refs + ItemLotParam_enemy/map row existence). Result reconciles EXACTLY
with the [RESIDUAL-SRC] totals: **51 ACCEPT + 44 RECOVERABLE.**
- **35 enemy = MIS-LABEL phantoms** — orphan ItemLotParam_enemy lots, referenced by NO NpcParam (ERR +
  full table + offline paramdef + VANILLA, all 0). Corpse/EMEVD-scripted loot the bake wrongly tagged
  Enemy (mapgenie shows them on "bodies"). NOT recoverable; items appear elsewhere. ACCEPT. [[msbe-enemy-loot-offsets]]
- **16 treasure = debake-gap** — real ItemLotParam_map corpse loot, chain absent from the mod's loot
  linkage. ACCEPT (~0.4%).
- **★ 29 unknown + 15 emevd = 44 REAL map-lots NOT covered (THE NEW LEVER)** — all lotType 1, all EXIST in
  ItemLotParam_map, no NpcParam (correct — they're chest/ground/EMEVD loot). The bake left 29 source-Unknown
  (pre-provenance) + tagged 15 Emevd. Concentrated in DUNGEONS (m16/m31/m42; e.g. the m31_8_0 sequence
  31080700/710/720 = one chest) + the 15 emevd incl. Reforged 5/5 + Key Crystal Tears 4. The disk treasure
  pass covers MSB Treasure EVENTS + the emevd pass covers template awards → these 44 fall through (likely
  CHESTS = MSB Asset w/ pickUpItemLotParamId, or EMEVD-direct rewards the template list misses). **Recovering
  them drives baked 133→~89.** NEXT: offline-trace a few (e.g. lot 31080700 m31, the emevd Reforged lots) to
  find the placement mechanism, then extend the right disk pass. Tools (all on master):
  _probe_residual_family.py + _probe_item_sources.py + the [RESIDUAL-ROW] dump.
  **★★ THE 15 EMEVD LOTS FULLY TRACED 2026-06-26 (branch feat/emevd-treasure-dup-debake) — split into
  EXACTLY 2 root causes, NOT a missing template (kEmevdTemplates already has 90005300/301).** Tools:
  tools/_probe_emevd_lots.py (lot→event/template), _probe_emevd_entity.py (entity→MSB tier), _probe_emevd_join.py
  (replicates the runtime join offline: emevd direct award + _00-enemy lookup + treasure_lots-dup check).
  - **(A) treasure_dup — 7 lots (1036490010..014 Reforged m60_36_49 chest, 2048400020/021 m61) ✅ FIXED.**
    The lot is BOTH an emevd direct award AND a `_00` MSB Treasure (the CHEST is the scripted award). The
    treasure pass (build_disk_loot_markers) already places base+sibling chain into `disk_lots`, but the emevd
    pass SKIPS it (`treasure_dup` → not in emevd_disk_lots) and the provenance guard (drops only Treasure/Unknown
    src) won't evict an Emevd-src row → baked Emevd twin survived = residual + a DOUBLE marker. Fix (map_entry_
    layer.cpp ~L1631): one extra dedup — drop a baked `Emevd` lotType-1 row when `disk_lots.count(lot)` (lot ids
    are globally unique → a disk treasure with this exact lot is the SAME pickup). **✅ RUNTIME-VALIDATED
    (build 00:34): `replaced 521 baked emevd` (was 514), `TOTAL baked=126` (was 133), residual 95→88, the 7 lots
    (Reforged 1036490010..014, m61 2048400020/021) gone, NO category regressed.** Branch feat/emevd-treasure-dup-debake.
  - **(B) entity-not-an-_00-enemy / LOD-tier scope — 8 lots (40524, 2045460500/501, 2050460500/501/510/511 +
    30510) ✅ FIXED, RUNTIME-VALIDATED.** The emevd award's ENTITY lives ONLY in a non-`_00` LOD tile (e.g. lot
    2045460500's entity 2045460200 = c5170 in m61_11_11_**02**, part name "m61_45_46_00-c5170_9000"; 40524's
    1148560200 in m60_24_28_**01**; 30510's DIRECT entity 1054560800 in m60_13_14_**02** — note 90005860/61/80
    are in BOTH kEmevdTemplates as a direct award AND bossFlagLot, so 30510 has a direct path too). The runtime
    parses `_00` only → the entity→pos join failed → no marker, no sibling walk. Fix = new
    `load_lod_award_entities(wanted)` (loot_disk.cpp): caller (map_entry_layer.cpp ~L1496) computes
    `missing_award_entities` = direct (lotType-1, !bossReward) award entities ∉ known_entities, scans the non-`_00`
    tiles' Enemy section ONLY for those ids, appends to disk_enemies — AFTER build_disk_enemy_markers +
    known_entities/entities_by_tile are built, so ZERO LOD phantoms in the enemy pass or boss/setter resolution.
    First-occurrence wins (mirrors the bake's entity_to_pos); LOD-tile pos = what the bake used. **RUNTIME (build
    00:44): `LOD award-entity scan: 7/138 resolved (293 tiles parsed)` [+~25ms, MSB parse is cheap — build.disk_loot
    is 44ms for 651 _00 tiles], `replaced 529 baked emevd` (was 521), `TOTAL baked=118` (was 126), residual 88→80,
    emevd 8→0 — Key-Crystal-Tears + Loot-Dragon-Hearts flipped 🟢, NO regression.** **EMEVD LEVER NOW FULLY
    EXHAUSTED (15/15 recovered).** Both fixes on branch feat/emevd-treasure-dup-debake (commit d2fd2a7 + the LOD
    commit). **THE 29 "unknown" chests — INVESTIGATED 2026-06-26, NOT cleanly recoverable, ACCEPT.** Task
    hypothesis (chests = MSB Asset w/ pickUpItemLotParamId) REFUTED: tools/_probe_unknown_chests.py shows 0/29
    are asset-pickups — all are EMEVD-scripted (28 emevd + 1 no-ref). 6 use KNOWN engine templates
    (90005300/555/750/880) whose entity arg is a non-positionable value/ASSET (Enemy-only join + the offline both
    miss them → stayed source=Unknown); ~22 use MAP-SPECIFIC per-tile templates (1034432261, 31082770,
    1047372200…) with INCONSISTENT (entity,lot) layouts. **A "m60 chest family" pass was BUILT then REVERTED:**
    the tightest structural rule (callee≥1e9, entity@idx2, lot@idx(n-2), lot-encodes-own-tile) is NOT chest-specific
    — tools/_probe_m60_chest.py proved it matches **395 distinct m60 lots** (≈ALL m60 scripted awards), 383 already
    covered by the treasure/enemy/emevd passes. The 12 residual chests are STRUCTURALLY INDISTINGUISHABLE from the
    383 covered ones → any isolating rule over-emits ~395 (double markers, wrong positions) or degenerates to
    hardcoding per-tile template ids (= baking). **VERDICT: accept the 29 (~0.4% of map), same class as the 16
    treasure debake + 35 enemy mislabel. The clean no-bake LOOT levers are EXHAUSTED at baked 118.** Probes kept:
    _probe_unknown_chests / _probe_chest_positionable / _probe_m60_chest. Further reduction = the WORLD-FEATURE
    backlog (Interactables 23, Kindling 5…), NOT loot.
  **★ IN-WORLD DIAG (merge feat/overlay-baked-only, 2026-06-26): overlay toggle `baked_only`** (config + ImGui
  checkbox "Baked-only (diag: show just the no-bake residual)"). Draws ONLY `Marker::source==Baked` markers =
  exactly the surviving residual (disk/live twins already deduped at finalize); early cull in the map_renderer
  draw loop. COMBINE with the per-category/section toggles to isolate a family's residual on the map (e.g.
  Baked-only + Equipment) → fly the world + eyeball each leftover spot: real loot the live passes miss (coverage
  gap → recover) vs a phantom/stale spot (bake bug → drop). The visual complement to the offline probes.
  Persists via Save to INI, default off.

**★★ WHERE THE 322 LIVES (residual map, scoreboard COVERAGE 17:53 in-game):**
- **DEBAKE-GAP 16** (was 328) = the genuine treasure residual, ALL m60: ~10 corpse weapons (key 1xxxxxxxx,
  whole ItemLotParam chain absent from disk = position-clip / in items_database but not loot_lot_linkage,
  the old "21 residual" from the DummyAsset doc) + 6 the window-proxy tagged sibling but the contiguous walk
  correctly rejects (param gap / repeatable flag). ✅ ACCEPTED residual (~0.4% of the treasure slice).
- **Reforged boss pieces — OFF-BAKED, COMPLETE 2026-06-25 (merge 4dce24d, feat/emevd-boss-pieces).**
  baked 322→198; Ember 19→0 (🟢), **Rune 114→9** (runtime-validated build 19:50). These are common.emevd
  BOSS-KILL awards (NOT a simple lot, NOT items_database staleness — items_database is correct, live lots all
  resolve). Mechanism: templates **90005860/61/80** init args = defeatFlag@8 (X0_4), bossEntity@16 (X8_4),
  baseLot@24 (X16_4) — we parse (defeatFlag, baseLot) because **defeatFlag == the boss MSB EntityID**
  (Reforged convention, ~83/92 binds — use it directly, else MAP-SCOPED setter-candidate join); the actual
  piece is **baseLot+2** (an ItemLotParam_**map** chain, lotType 1 — NOT enemy; getItemFlagId is the gray
  flag). event-1200 (per-map dungeon bosses): common.emevd binds flags **9200-9281 → lots 20000-20810**
  (InitializeEvent(.,1200,flag,lot,..)); the per-tile emevd event 300X2800 sets the flag on boss death; piece
  = lot+2. parse_emevd_full→EmevdParse.bossFlagLot; load_emevd_awards→DiskEmevd.bossReward; build_disk_emevd_
  markers walks base..base+8 (lotType 1) for goods 800010/850010, emits under the Reforged category at the boss
  pos, gray via getItemFlagId. ev1200 piece emit is ADDITIVE (consuming all lotType-2 regressed ~135 baked).
  Dedup baked c-model twin by (tile+partname) AND (tile+__BOSSRUNE/EMBER__).
  **★★ THE per-dungeon (m30/m31/m32) FIX (fix commit 6f7d2e9) = MAP-SCOPE the setter→boss candidate join.**
  Root cause: load_emevd_awards intersected each setter event's candidate 4-byte windows with a GLOBAL
  knownEntities set (all disk MSB enemies, every map). A lower-numbered boss-like EntityID from a DIFFERENT
  dungeon that coincidentally appears as a 4-byte window in this event then won the "boss-preferred, lowest"
  pick → piece placed in the wrong tile → baked twin not deduped → residual. The offline bake never hit this:
  extract_all_items intersects per-map `valid_entities` (map_to_entities[map_name]). Fix mirrors it: EmevdSetter
  gains `mapTile` (area<<16|gx<<8|gz, stamped from the emevd filename); the caller builds `entitiesByTile`;
  a `resolve_boss` lambda scopes candidates to mapTile (else global for common/m60.emevd). Offline-proven
  against regulation.bin+real MSBs/emevd (3 replications: join+walk+dedup = 48/49 with bug, 49/49 per-map).
  **NOT a class of bug — ISOLATED to mechanism B** (the only join that picks an entity by heuristic from
  candidate-scanned values ∩ a global set; both sites — ev1200 + boss-reward fallback — now map-scoped).
  Every other emevd pass reads the entity at a FIXED template arg offset (Direct/HostileNPC 90005792@+20/
  HeroTomb 90005683@+12/Paintings 90005632@+12/16/Gestures 90005570@+16) → explicit, globally-unique, immune.
  **ACCEPTED RESIDUAL (9, all m60):** 8 overworld (c4980/c3150/c4950) no-entity (flag != EntityID, different
  convention) + 1 cross-tile (m60_13_14 ref's m60_54_56 — map-scoping correctly leaves it residual, the bake
  too). All temp diags ([EV1200-*], [BOSSBIND], [EV1200BIND], [EMEVD-ARGDUMP], [PIECE-RESIDUAL]×2) REMOVED.
- **World - Interactables 6** (was 88 → 23 → 6) — ✅ AEG099_090 seals OFF-BAKED 2026-06-25 + ✅ EXTRA PUZZLES
  OFF-BAKED 2026-06-26. The 23 residual were the bespoke non-template "extra puzzles"; 17 now off-baked (RUNTIME-
  VALIDATED build 10:41, baked=6 disk=96, flagged 102/102): **Sellia chalices AEG099_047 (3) + Siofra lanterns
  AEG237_055 (14)** — both `_00` assets. Mechanism = ZERO new C++, the SAME seal pattern: +5 bespoke event ids in
  kEmevdFlagTemplates (Sellia 1049392302/303 + 1050392303 ent@X0/flag@X4; Siofra 12022601/621 anchor-asset@X4/
  flag@X0; offsets from tools/extract_seal_puzzles.py:_EXTRA_PUZZLES) + 2 rows in world_feature_assets.py
  (seal_emevd self-gate, text 800000000+9520 "Light flame") + regen generate_world_feature_models.py (standalone,
  NO full-pipeline regen → no goblin_location_alt wipe). Predicted+matched 17 via tools/_probe_extra_puzzle.py.
  **Residual 6 = 4 Snow Town seal-release statues (AEG110_029, event 1048572370) + 2.** NOTE: memory had the
  model↔puzzle labels CROSSED — corrected: AEG237_055=Siofra lanterns (m12_02, _00), AEG110_029=Snow Town statues
  (m60_24_28, LOD), AEG099_047=Sellia chalices (m60_49/50_39, _00).
  **★★ SNOW TOWN STATUES OFF-BAKED + RUNTIME-VALIDATED 2026-06-26 (build 11:13) — branch
  feat/world-snow-statues-lod (commits 26f50c8 feat + 65dbc26 cross-tile fix + ac2855a doc), baked 96→92.
  `LOD feature-asset scan: 4 placements`, Interactables baked 6→2 / disk 96→100, `category 64 dropped 100
  baked twins (cell-dedup)`, flagged 102/102, zero other category changed. UNMERGED (<user> merges --no-ff).** category-A, tier-blocked: AEG110_029 lives ONLY in the LOD
  supertile m60_24_28_01 (4 placements, 0 in _00; part name "m60_48_57_00-AEG110_029_…"), which the _00-only asset
  enumeration skips. Fix = the ASSET analogue of load_lod_award_entities (the emevd LOD fix): new
  `load_lod_feature_assets(wanted)` (loot_disk.cpp) scans non-_00 tiles' Asset section for the wanted aegRows, marker
  tile from the cross-tile part-name PREFIX (m60_48_57 → grid+pos lands right, SAME as the bake's generate_seal_puzzles.py;
  NOT the LOD file's m60_24_28). Caller (map_entry_layer.cpp ~L1449) appends to disk_collectibles AFTER
  build_disk_collectible_markers, BEFORE build_disk_world_feature_markers (never counts as loot; other world-feature
  passes filter by their own model → inert). Generic lod_scan bool added to WorldFeatureModel (struct+generator+editorial
  row, <user> chose generic over hardcoding 110029). +1 kEmevdFlagTemplates {1048572370, 8, 16, 20} (entity@X0_4, lit
  flag@X8_4, arglen 20) → SealEmevd self-gate grays via flag 1048570370+. Probe tools/_probe_snow_statue.py confirmed
  4/4 (entity→flag join + LOD tier + prefix). EXPECTED: Interactables baked 6→2, total baked 96→92. ⚠ minLen=20 derived
  = furthest field off (flag@16) + 4. The residual +2 Interactables remain (un-probed; next if chasing Interactables→0).
  **⚠ FIRST runtime test returned `LOD feature-asset scan: 0 placements` — GOTCHA: `aeg_row_from_name` (msbe_parser)
  is START-ANCHORED ("AEG…"), but cross-tile LOD proxies are named "m60_48_57_00-AEG110_029_2000" (tile prefix) → the
  parser returned row 0 and SKIPPED them before r.assets. FIX (commit 65dbc26): opt-in cross-tile parse — a `crossTile`
  arg on aeg_row_from_name (accept the model token after a "-AEG") + a `crossTileAssets` flag on parse_msb (default
  false), set TRUE only by load_lod_feature_assets. Kept the _00 passes strict ON PURPOSE: tools/_probe_dash_aeg.py
  found 4 _00 assets carry a cross-tile name (AEG099_170/009 in m11_05 w/ EntityIDs = potential pickups); a GLOBAL
  generalization would newly emit ~2 collectible markers = a regression. GameEditionDisable=0 on all 4 statues (probe)
  → the name parse was the SOLE blocker. Rebuilt+redeployed; SECOND runtime test pending. LESSON: ER LOD-supertile part
  names carry a "m{tile}_{lod}-" cross-tile prefix; any name-based model parse over non-_00 tiles must handle it.**
- **🔴 baked-only DEAD-ENDS (now just KINDLING + Ammo):** ~~Material Nodes 11~~ ✅ FIXED (identity dedup, see
  above — was a dedup-KEY bug, not residual). (1) **Kindling 5 — DISK-RECOVERABLE (category-A), not the
  dead-end we thought.** The ONLY remaining 🔴. Earlier "heap-only, no position" verdict was about live STATE;
  the POSITION is plainly ON DISK: tools/_probe_kindling_region.py found the 5 as MSB **SFX regions** in
  m60_45_37_00.msb named `KindlingSpirit_0001..0005` (eids 1045373501..505) WITH positions (0001=-27.7,17,56.1;
  also a `KindlingSpiritX_*` lit-variant set 511..515). **CHEAP off-bake:** extend the region parser
  (msbe `secs[SEC_POINT]`, already read for spirit-springs) to the SFX subtype + name-prefix `KindlingSpirit_`,
  emit the 5 under WorldKindlingSpirits, category-wipe finalize; graying stays via the live goblin_kindling.
  **✅ SHIPPED + RUNTIME-VALIDATED 2026-06-26** (build 10:27): region parser keeps `KindlingSpirit_` SFX regions;
  the baked loop OVERRIDES each baked kindling marker's pos from the disk region of the same object_name + flips
  source→DiskMSB (row_id/identity/graying via goblin_kindling untouched). `re-sourced 5`, Kindling baked=0 disk=5,
  census 5/5, baked 118→113, **🔴 baked-only count 1→0** (zero baked-only categories left). See
  [[residual-irreducible-strategy]] (the lens's worked example: assumed B, checked disk first, found A). (2) Ammo 3 =
  bake edge cases (2 EMEVD lots 20650/20810 resolve to WEAPONS, mis-filed under Ammo + 1 uncovered treasure).

**The scoreboard has an `icon` column** (symbol/atlas N%/circle/none + ⚠ faint <25%) — see [[world-feature-
msb-identities]] for the Stakes "faint icon ≠ bug" lesson (Stakes = `atlas 19% ⚠`).

**★ Material Nodes migrated (merge 734ab95) + 2 disk-pass bugs fixed (merge 0878ee4), 2026-06-25.**
Replaced the collectible pass's `_8xx` model-range heuristic with the LIVE native gather signal
`goblin::aeg_is_gather` = `AssetGeometryParam.isEnableRepick` (**byte 0x3c BIT 5 / mask 0x20** —
EMPIRICALLY pinned; bit 6/0x40 is isBreakOnPickUp). The geom-dedup drops the baked LootMaterialNodes
(→11 offset-residual). Two bugs surfaced (<user> spotted both visually), both FIXED:
- **Phantom pieces:** the piece check `sub = aegRow%1000; sub==821/822` matched EVERY AEG group's _821/_822
  (AEG023_822=108, AEG230_821=53, …) → 592 fake "Rune/Ember Piece" markers ("Éclat calciné" clusters).
  Fix = match the full `aegRow == 99821/99822` (AEG099 only). Rune disk 1579→1118, Ember 410→279.
- **★ 16k gather LEAK:** `aeg_is_gather` read byte 0x3c **bit 6 (0x40) = isBreakOnPickUp** (set on ~18k
  breakable pickups) instead of **bit 5 (0x20) = isEnableRepick** → Crafting Materials disk **16800**
  (vs the ~1464 REAL gather, MSB-confirmed = ~the bake's 1455). My offline `_probe_aeg_offsets.py` *computed*
  the offset and mis-packed the dummy8 Reserve_2 bit. Fix = mask 0x20 (empirically pinned, see
  [[re-offset-validation]]). **There was NO curation problem — the "full gather universe" was a wrong-bit
  artifact.** Crafting 16800→**1697**, disk total 23379→6938, flood gone, baked still 1908.
**Counter note (still useful):** `[COVERAGE]` counts RAW g_buckets per-placement; the ImGui badge
(`refresh_overlay_census`) counts only completable spots (distinct persistent flags) and EXCLUDES respawnable
gather (lot_backed + no flag). They measure different things BY DESIGN — the scoreboard upgrade (item 5) will
surface both. 17 🔴 baked-only / 29 🟡 partial / 17 🟢 off-bake of 63. The baked-only backlog = (1) GEOM/SFX-tracked pieces (Material Nodes 1455,
Rune Pieces 1227, Ember Pieces 298 — non-lot-backed by design) + (2) World features (Stakes 439,
Quest NPC 344, Summoning 245, Interactables, Spirit Springs, Hostile NPC, Imp, Maps, Paintings,
Kindling — no disk loot source) + (3) small loot (Gestures 7, Great Runes 6, Ammo 3).

**RESOLVED 2026-06-25 — PARTIAL double-count.** `Loot - Material Nodes` baked=1455 (lotId=0,
GEOM/SFX-tracked, from `generate_material_nodes.py` over AEG099/463 one-time gather nodes) splits
by model sub-number: **_8xx (800-899) = 546 rows ARE double-counted** with the disk collectible
pass (which emits ONLY sub 800-899, classified by item → Crafting Materials/Consumables/…). Because
the baked rows have lotId=0, the disk lotId-replace can't drop them → a _8xx node draws TWICE on
the map (Material Nodes + Crafting toggle both on). The other **909 (_6xx/_7xx/_9xx) are DISTINCT**
— the disk pass deliberately skips sub<800/>899 as clutter (anti-flood). FIX (deferred): drop the
_8xx from generate_material_nodes.py (or runtime-skip baked LootMaterialNodes _8xx when
loot_collectibles is on) → removes 546 from baked-remaining + fixes the duplicate marker, keeping
the disk pass's more accurate per-item classification. See [[aeg-collectible-source]].
