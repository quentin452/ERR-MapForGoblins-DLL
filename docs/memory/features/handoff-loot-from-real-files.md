---
name: handoff-loot-from-real-files
description: "HANDOFF — state of the \"loot markers from the mod's REAL MSB files, no committed bake\" migration: done / caveats / missing / next steps"
metadata: 
  node_type: memory
  type: project
---

# HANDOFF — loot from the active mod's real files (drop the committed bake)

> **★ 2026-06-25 SESSION (branch feat/msbe-entity-recover-dummy, 10 commits fcd7736→6b4d801, UNPUSHED —
> <user> pushes & runtime-tests).** Cleared the 6 polish TODOs: #1 tooltip names (preload all GoodsName,
> fcd7736), #2 collectible density (reuse show_* category toggles + classify_item_live refine, 10a473a),
> #3 ImGui perf (deferred per-marker label — render 1.48ms@4865 markers, NOT a bottleneck; 50fps=GPU-bound
> 1650Ti; 8b7560d), #4 embed DejaVu Sans TTF (drop C:\Windows\Fonts; e16ab63 + doc 353ba26), #5 "spots
> restants" census (exclude respawning flag-less nodes — no native counter exists; 7601e79), #6 arch-debt
> STEP 1 = loot_source provenance tag (116e07e infra + c31b54b ERR regen + 72ef36e DEBAKE-GAP diag +
> 6b4d801 DEBAKE-RECOVER diag). See [[aeg-collectible-source]] + [[imgui-unicode-font]].
> **#6 DE-BAKE VERDICT (this session's big finding):** the de-bake gap = **328 baked Treasure rows the disk
> can't reproduce**, ≈ the **346 partBucket=NONE treasures** (AEG099_630/090 corpse loot: Fire Monk/Cuckoo
> Knight armour sets, Crimson Hood) — bound to their asset by an OFFLINE heuristic (enrich_fallback_with_
> emevd), NOT a clean MSB structure the parser reads. Runtime diag PROVED widening the collectible filter
> recovers 0 (the pickUpItemLotParamId lot ≠ the corpse's armour). So **~328 must STAY BAKED as a curated
> residual (~9% of the treasure slice).** Clean no-bake path = de-bake ONLY partBucket='live' (3637, disk-
> proven), keep partBucket=NONE baked (encode in provenance: live→Treasure, none→CuratedResidual).
> **★ 2026-06-25 LATER#2 — ENEMY DISK PASS BUILT (commit df8ba66, UNPUSHED; DLL deployed; flag OFF
> by default so default behaviour unchanged).** The hard RE is DONE + pinned (see
> [[msbe-enemy-loot-offsets]]): MSB Enemy part type==2, `NPCParamID=*(*(part+0x68)+0x0c)`
> (26277/26277 vs SoulsFormats), pos@+0x20; live NpcParam row 736, itemLotId_map@0x34 (pref,lotType1)
> / itemLotId_enemy@0x30 (lotType2). Full pass wired behind config `loot_enemy_drops`: msbe_parser
> Parts.Enemies (wantEnemies) → goblin::npc_loot_lot(npcId,&lt) (call_once worker-safe) → loot_disk
> DiskEnemy out-param → map_entry_layer build_disk_enemy_markers + an ENEMY provenance guard (drops
> matching baked LootSource::Enemy rows) + [LOOTDISK] enemy-drops diag. Built clang-cl OK, deployed.
> **★ BLOCKER FOUND — the pass emits the RAW enemy universe (~25608 placements w/ a lot), NOT the
> curated 119.** items_database has 25608 source='enemy' rows (= the "25608" number); the bake cures
> 25608→119 via the pipeline's ~60 LOOT_CATEGORIES notability filters (golden runes ≥ threshold,
> merchant bell-bearings, named weapons/armour, larval tears…). The clutter (17519 consumable drops:
> Sliver of Meat ×3480, Thin Beast Bones ×2902, mushrooms…) matches no notable category. There is NO
> clean game-data notability flag — same arch-debt as the collectible clutter + treasure corpse
> residual. So loot_enemy_drops=true FLOODS the map (~25k markers; category toggles trim, e.g.
> consumables-off hides 17519, but it's the raw universe). Matching 119 = port the LOOT_CATEGORIES
> filters to C++ (big) OR find a runtime notability signal. **DECISION DEFERRED (<user> 2026-06-25):
> committed the pass + RE as-is (flag off = enemy stays the curated-119 baked residual, pass available
> opt-in for raw coverage). NEXT OPTIONS he named: (a) Ghidra static, (b) RPM runtime, (c) continue
> datamining to understand WHEN/WHERE the runtime does loot-notability curation** (is there an in-game
> signal — non-respawn flag, getItemFlagId, unique-item marker — that distinguishes the 119 notable
> drops from the 25k clutter, so the pass could filter live instead of porting the pipeline policy?).
> See docs/re/windows_enemy_loot_nobake_analysis.md + [[msbe-enemy-loot-offsets]].
> **★★ BLOCKER SOLVED — notability signal datamined (commit ce31e91, UNPUSHED, deployed). The game's
> own one-time-loot flag separates notable from clutter, NO LOOT_CATEGORIES port needed.**
> tools/datamine_enemy_notability.py over the 25608-enemy universe: the 119 baked enemy lots are ALL
> `getItemFlagId != 0` (one-time persistent obtain flag; respawning clutter = flag 0) AND placed on a
> UNIQUE enemy (lot appears on exactly 1 placement). The combined filter `flag!=0 && placements==1` =
> **349 lots = a PERFECT SUPERSET of the 119 + 230 notable drops the bake MISSED** (Blaidd's/Ronin's
> armour sets, Hoslow's Petal Whip, Patches'/Miriel's Bell Bearing, Inseparable Sword+Twinned set,
> Irina's Letter, Dancer's Castanets…). Both signals are LIVE (resolve_loot_flag exists; placement-count
> counted in the disk pass). build_disk_enemy_markers now filters on them → ~349 notable markers (not
> 25k), STRICTLY BETTER coverage than the curated bake. So the enemy slice is genuinely no-bakeable +
> improved. NEXT to validate runtime: <user> runs loot_enemy_drops=true + diag_loot_pos → watch
> `[LOOTDISK] enemy drops: ~349 notable markers emitted` + the skip breakdown (swarm/no-flag). If clean,
> loot_enemy_drops can become a real shipped source (and possibly default). Same flag!=0 idea may also
> recover the 230-bake-miss for TREASURE later. See docs/re/windows_enemy_loot_nobake_analysis.md §notability.
> **★★ RUNTIME-VALIDATED + TUNED (commits ce31e91→344d734, deployed). The "349" was RAW flags; runtime
> truth ≈ 124.** <user> ran loot_enemy_drops: first build emitted 99 (filter A: flag&&placement==1).
> Diagnosed via tools/sim_enemy_pass.py (mirrors the runtime over the disk Parts.Enemies parse, the
> AUTHORITATIVE reproduction — items_database differs): (1) resolve_loot_flag already zeroes
> repeatable/temp flags (>=0x40000000 or -1 = golden runes/gloveworts), so that IS the notability cut —
> the "349" dropped to ~101-124 once applied. (2) Switched to FILTER B (<user>'s pick): persistent
> getItemFlagId, DEDUP BY LOT → **~124 notable markers** (vs 101 for placement==1). Covers 81/119 baked
> by lot + ~43 the bake MISSED (Blaidd's/Ronin's sets, Hoslow's Whip, bell bearings). (3) The other 38
> baked stay baked via the lot-exact guard, NO LOSS: 35 use a SIBLING ItemLotParam row (npc→base lot
> e.g. 333062020, bake→item row 333062021 in the same sequence; exact match needs ItemLotParam-chain RE,
> marginal; only ~3 actual duplicate overlap) + 3 are m61 DLC one-time drops (Blessed Bone Shard ×2,
> **Iris of Occultation** quest item) whose flag is in a high DLC range (~2.049e9) resolve_loot_flag
> wrongly treats as repeatable — so the disk pass skips them, the bake keeps them (correct; do NOT
> drop-all baked Enemy or you lose Iris of Occultation). VERDICT: enemy slice is no-bakeable + improved;
> ~124 disk + 38 curated-residual baked, ~3 dupes. NEXT runtime check: loot_enemy_drops=true → watch
> `[LOOTDISK] enemy drops: ~124 notable markers emitted`. New tools: sim_enemy_pass.py,
> datamine_enemy_notability.py, probe_enemy_npc_offset.py, probe_npc_lot_offset.py.
>
> **★★ 2026-06-25 LATER#3 — EMEVD + CORPSE RECOVERABILITY DATAMINED (zero-bake floor corrected; tools
> datamine_emevd_recover{,2}.py, UNPUSHED — not yet committed at handoff time).** Earlier "Emevd
> unplaceable / ~460 irreducible" was WRONG. ALL 529 baked Emevd rows already have a position from an
> MSB part (521 c-prefix ChrIns + 8 cross-tile, 0 posless); the lot→entity link is in the on-disk EMEVD
> files (event/*.emevd.dcx, bank-2000 event inits carry (entity_id, lot_id) at template offsets, see
> extract_all_items.py:646 TEMPLATE_EVENTS). **Emevd 529 → ~100% recoverable** with a runtime EMEVD
> parser: 373 DIRECT (init co-carries lot + a reachable MSB EntityID/group) + 156 via a sequence-base
> lot (ItemLotParam-chain rows e.g. 20081-85 Tree Sentinel set, base 20080 is EMEVD-bound) + 0
> unrecoverable. **Corpse Treasure residual (312, partBucket≠live) → NOT EMEVD**: only 47 recoverable,
> **265 are the enrich_fallback heuristic bindings** (AEG099_990/090 corpses: Sign of the All-Knowing,
> Juvenile Scholar Robe…) = the TRUE irreducible residual. **CORRECTED zero-bake-zero-loss floor ≈ 265
> corpse markers (~6% of the loot slice), not ~788.** Path to it, ROI-ordered: (1) RUNTIME EMEVD PARSER
> (C++; unblocks all 529 Emevd, 373 direct — the big lever; EMEVD = events→instructions→argblobs, filter
> bank==2000 inits whose eventId∈templates, read entity/lot at fixed offsets, join EntityID→MSB part
> [+0x60 offset already pinned]→pos), (2) ITEMLOTPARAM-CHAIN RE (base lot→item row; SHARED: enemy 35 +
> emevd 156 + corpse 30), (3) DLC flag-range fix (enemy 3). The ~265 corpse stay baked (curated
> residual). DECISION PENDING: build EMEVD parser (big lever) vs ItemLotParam-chain first (smaller,
> shared). See docs/re/windows_enemy_loot_nobake_analysis.md §5b.
>
> **★★ 2026-06-25 LATER#4 — EMEVD DIRECT-TEMPLATE PASS BUILT + DEPLOYED (commit 624b040, UNPUSHED;
> flag OFF by default → default behaviour unchanged). The runtime EMEVD parser is DONE.** New config
> `loot_emevd_drops`. Chain, all live/no-bake: `msbe::parse_emevd` (NEW; 64-bit EMEVD layout PINNED —
> eventCount@0x10, eventsOffset@0x18, instrTableOffset@0x28, argsOffset@0x78; event stride 0x30,
> instruction stride 0x20 {bank@0,id@4,argLen@8,argOffset@0x10 int32}; arg[4]=invoked eventId; filters
> bank-2000 inits whose eventId ∈ the 13 TEMPLATE_EVENTS, extract_all_items.py:646; reads (entity,lot)
> at the per-template offsets) → `loot_disk::load_emevd_awards` (parses event/*.emevd.dcx, the SIBLING
> of map/MapStudio) → `build_disk_emevd_markers` (joins EntityID→MSB Enemy part via the NEW
> `msbe::Enemy.entityId` = part+0x60 entity sub-struct, EntityID@+0x00, SAME offset as the treasure
> path; dedup by (entity,lot); resolve lot LIVE as ItemLotParam_map lotType 1; emit at the enemy
> block-local pos). Provenance guard drops baked LootSource::Emevd rows it covers (parallel to the
> enemy guard, in build_buckets_impl). **VALIDATION: tools/probe_emevd_format.py pins the layout +
> proves the pure-bytes parse == SoulsFormats 500/500 over ALL ERR event files; the C++ parse_emevd is
> a line-for-line port.** End-to-end offline reproduction (probe + datamine_emevd_recover.py AGREE):
> 500 raw awards → 318 unique (entity,lot) positioned markers → **307/529 baked Emevd lots COVERED**
> (de-baked), 222 stay baked. ⚠️ **NUMBER CORRECTION: LATER#3's "373 direct" was WRONG — the validated
> direct-template tier is 307.** The 222 residual = sequence-base ItemLotParam-chain siblings (the
> `200X1` smithing-stone rows whose base `200X0` is the EMEVD-bound lot) + `common.emevd` event-1200
> flag→lot drops (Lhutel the Headless et al.) — both OUT OF SCOPE for this direct pass (future:
> ItemLotParam-chain RE [SHARED w/ enemy 35 + corpse 30] + the event-1200 mechanism). Also fixed: the
> live name-space tooltip preload now ALSO gates on loot_enemy_drops + loot_emevd_drops (enemy was
> missing). Built clang-cl + deployed. **NEXT runtime check (<user>): loot_emevd_drops=true +
> diag_loot_pos → watch `[LOOTDISK] emevd drops: ~318 markers emitted` + `replaced ~307 baked emevd
> rows`.** See docs §5b + [[msbe-enemy-loot-offsets]].
> **★★ RUNTIME-VALIDATED 2026-06-25 (loot_emevd_drops=true + diag_loot_pos, <user>'s run).** Live
> log confirms: event dir auto-resolved to `…\mod\event` (sibling of map\MapStudio); **500 template
> awards** parsed from 517 EMEVD files (0 KRAK skipped) = EXACT match to SoulsFormats; **308 EMEVD
> markers emitted** (filtered: 163 entity-not-an-msb-enemy, 23 (entity,lot)-dedup, 2 treasure-dup, 4
> unclassified); **300 baked LootSource::Emevd rows replaced/de-baked**. Matches the offline
> prediction (308 vs ~318 markers, 300 vs 307 covered — the ~7 gap = the 2 treasure-dup + 4
> unclassified the offline repro didn't filter). NO crash from the new code (the session's crash_*.txt
> is a 100%-eldenring.exe first-chance AV, benign — overlay kept running past it). Enemy slice de-bake
> = SHIPPED + improved earlier; EMEVD direct slice = SHIPPED + validated now. Residual baked loot floor
> per §5: Emevd-sequence-base/event-1200 222 + Treasure-corpse ~265 ≈ ~487 curated rows. NEXT de-bake
> levers (future): ItemLotParam-chain RE (recovers emevd 222-tier siblings + enemy 35 + corpse 30) +
> the common.emevd event-1200 flag→lot mechanism (Lhutel et al.).
> **★★ 2026-06-25 LATER#5 — EMEVD residual datamined + MECHANISM B SHIPPED (commits 37ab809 datamine,
> 6a386bb mechanism B; UNPUSHED; DLL deployed).** `tools/datamine_emevd_residual.py` characterized the
> 222 residual = **55 event-1200 (boss drops) + 167 sequence-sibling + 0 unrecoverable**.
> - **(B) event-1200 — BUILT + folded into loot_emevd_drops.** `msbe::parse_emevd_full` also extracts
>   `RunEvent(2000:00,callee=1200,[flag,lot])` (flag→lot, common.emevd only) + each
>   `SetEventFlag(2003:66/69,.,state=1)` event's {flags, entity-candidates}; `load_emevd_awards(knownEntities)`
>   joins: setter flag→lot + candidate ∩ MSB enemy EntityIDs (boss-pref eid%1000∈800..899) → position,
>   lot via ItemLotParam_enemy (lotType 2, _map fallback). DiskEmevd got a lotType hint. **Validated
>   pure-bytes==SoulsFormats: 59 bases / 55 residual covered** (probe_emevd_format.py). So
>   loot_emevd_drops now de-bakes **~362** (307 direct + 55 ev1200). NEXT runtime check: watch
>   `[LOOTDISK] emevd drops: N direct + ~55 event-1200`.
> - **(C) sequence-siblings (167) — DEFERRED (<user>: "build B first, then figure out how to avoid the
>   C flood").** C does NOT cleanly reproduce the bake: emitting all flag>0 non-empty siblings = +478,
>   of which **316 are over-emit** the bake dropped via its ~60 LOOT_CATEGORIES rules (same arch-debt as
>   enemy/collectible clutter — no clean runtime signal; over-emit dominated by repeated runes/stones).
>   Variants measured: ev1200-base-siblings only = +61 over-emit for 80/162 covered (cleaner);
>   direct-base-siblings = +255 for 82 (worst); all = +316 for 162. **OPEN: how to bound C's flood**
>   (per-category show_* toggles like collectibles, OR ev1200-only, OR port LOOT_CATEGORIES). The
>   sibling-walker is SHARED with the enemy-35 + corpse-30 residuals. New tool: datamine_emevd_residual.py.
> **★★ C "FLOOD" DEBUNKED + 0-IRREDUCIBLE (datamine_emevd_residual.py w/ real names from item.msgbnd;
>   commit 25642d9 + doc).** The +316 C over-emit is NOT clutter: **181 = Rune Piece (goods 800010) +
>   Ember Piece (850010)** already on the map via the Reforged GEOM category (double-count → suppress by
>   category), **102 = EQUIPMENT the bake never showed** (full armour sets + weapons from named NPCs/bosses:
>   Ansbach/Igon/Millicent/Twinned/Omen/Verdigris/Raging Wolf/Hoslow/Iji + Ansbach's Longbow — runtime
>   STRICTLY BETTER, like enemy +43), **~33 = goods** (extra smithing stones/scadushards, toggle-controlled).
>   **The "5 unrecoverable" ARE recoverable** — Oracle Effigy + 3 Fortunes (flag 60320 bundle) + Viridian
>   Hidden Tear: all `live_classify=OK` (the live mod-agnostic classifier handles identity) AND all have a
>   PLACED base; the catch is they're CROSS-TABLE siblings (base in ItemLotParam_enemy, reward bundle in
>   _map). **So C must walk siblings in BOTH tables → EMEVD 529 = 100% no-bakeable, 0 irreducible.**
>   C PLAN (building now): per placed base (direct+ev1200), walk base+1.. in enemy AND map (map branch
>   stops at a treasure base), emit flag>0 + non-empty + NOT Rune/Ember (skip goods 800010/850010 →
>   Reforged), classify live → show_* toggles control density. SHARED walker w/ enemy-35 + corpse-30.
> **★★ MECHANISM C BUILT + deployed (commit 5335079, UNPUSHED; awaiting runtime validation).** New
>   primitive `goblin::lot_row_in_table(lot,lotType,&flag,&key)` = probe ONE ItemLotParam table (no
>   fallback) → exists?/notability flag (resolve_loot_flag semantics)/encoded item key. In
>   `build_disk_emevd_markers`, after each base award, walk base+1.. in BOTH tables (stop at gap; _map
>   branch also stops at a treasure base), emit each sub-lot that has flag!=0 + real item + isn't already
>   placed (base_lots/treasure/sib_seen) + isn't Category::ReforgedRune/EmberPieces (the 181 dedup),
>   classified live at the base position. Built clang-cl 12/12 + deployed. EMEVD A+B+C all on
>   loot_emevd_drops now. **NEXT (<user> relaunch): watch `[LOOTDISK] emevd drops: ~388 base + N
>   sequence-siblings` + `replaced ~500+ baked emevd rows` (was 355 with A+B).** If the sibling count or
>   the replaced count looks off, check the per-category breakdown (siblings route into normal buckets;
>   show_* toggles trim density). The lot_row_in_table walker is REUSABLE for enemy-35 + corpse-30.
> **★★ A+B+C RUNTIME-VALIDATED (commits 5335079 + the rune/ember-id fix; <user>'s run 07:51).**
>   `[LOOTDISK] emevd drops: 388 base (308 direct + 80 ev1200) + 160 sequence-siblings = 548 total;
>   155 rune/ember-skipped` → **replaced 512 baked emevd rows of 529 = 97% de-baked** (0→355 A+B→512
>   A+B+C). The 160 clean siblings = the ~102 boss/NPC armour sets + smithing-stone secondaries the bake
>   never showed. GOTCHA FIXED: rune/ember suppression by Category FAILED (Rune 800010/Ember 850010 are
>   GEOM-tracked → not in ITEM_ICONS → classify to generic goods, never Reforged) → 155 leaked as
>   wrong-position duplicates on bosses; now suppressed by goods id (ERR-specific, no-op elsewhere). No
>   crash from C (session crash_*.txt = the usual benign eldenring.exe first-chance AV). Remaining 17/529
>   (3%) = sibling lots whose flag resolve_loot_flag treats as repeatable + a few edge cases — not chased.
>   **EMEVD slice DONE: 512/529 no-bake.** lot_row_in_table is the reusable walker for the enemy-35 +
>   corpse-30 sibling residuals next.
>
> **★ 2026-06-25 LATER — ENEMY/TREASURE DATA-MINING DONE (this session, branch unchanged, UNPUSHED).**
> Full no-bake matrix computed + doc `docs/re/windows_enemy_loot_nobake_analysis.md` +
> `docs/re/enemy_markers_table.md` (the 119 enemy rows). Runtime diag `[ENEMY-MARKERS]` added
> (map_entry_layer build loop, gated diag_loot_pos; built clang-cl OK + deployed). **KEY FINDINGS:**
> (1) The **119 enemy markers** all resolve in items_database, all lotType 2, each has a ChrIns part
> `cXXXX_9YYY` + npcParam → **fully disk+param-derivable** via `MSB Parts.Enemies → NPCParamID →
> NpcParam.itemLotId_enemy/_map → ItemLotParam_enemy`, pos = enemy Part+0x20 (offline-proven in
> extract_all_items.py:533-567; same shape as treasure/collectible passes). Dist: m60 79, GoldenRunes/
> Low 59, BellBearings 19 (merchants), Gloveworts 11, equip ~17, bosses/named few. So enemy = NO-BAKEABLE
> *if a Parts.Enemies runtime pass is built* (NOT yet built). (2) **The loaded-loot walker is the WRONG
> tool** for enemy (question answered NO): MapIns+0x460 node only holds SPAWNED/post-kill loot, never the
> pre-kill enemy ChrIns binding — see [[fieldins-pool-registry-re]]. (3) **Treasure split confirmed:** of
> the 3639 baked Treasure, partBucket live **3325** (disk-reproducible) + NONE **312** (≈ runtime
> DEBAKE-GAP 328 corpse-loot) + reachable_dummy 2; NONE residual is 65% m60. (4) **Unknown 4366** = NON-loot
> map features (MaterialNodes 1455 + Rune/EmberPieces 1525 GEOM-tracked + Stakes/QuestNPC/Pools/etc.), only
> 29 lot-backed — out of the loot no-bake scope. (5) **VERDICT:** quasi-complete LOOT no-bake is reachable;
> genuine baked-loot floor = Emevd 529 (unplaceable) + Treasure corpse ~312-328 ≈ **840-860 curated rows**
> (~10%), +119 enemy until the pass ships. **NEXT = build the enemy disk pass** (msbe_parser Parts.Enemies +
> loot_disk NpcParam join; oracle = the 119 table). See [[aeg-collectible-source]].
> **★ EARLIER NEXT-SESSION note (<user>'s proposal): continue the data-mining — WHERE enemies/treasures are located**
> map enemy-drop + treasure placement more fully). ⚠️ **NUMBER CORRECTION (<user> caught this 2026-06-25):
> "25608 enemy drops" is the RAW offline enemy-lot universe (ItemLotParam_enemy / all enemy placements),
> NOT baked markers. The bake has only LootSource::Enemy = 119 actual enemy-sourced MARKERS (from the ERR
> provenance regen) — scattered in loot categories, no dedicated "enemy" overlay category. So the enemy
> slice is SMALL, not dominant. The overlay's "World - Bosses/Hostile NPC/Summoning Pools" rows are WORLD
> FEATURES (live param), not enemy drops.** Direction: can those 119 enemy markers be runtime-located (live
> ChrIns/FieldIns walk, see [[fieldins-pool-registry-re]] loaded-loot walker) vs forever-baked. ⚠️ REGEN GOTCHA still open: generate_location_
> overrides wipes goblin_location_alt.cpp to 0 on regen (task task_3c9902d0) — revert that file after any regen.


> **★ 2026-06-24 SESSION (branch feat/msbe-entity-recover-dummy, 14 commits a80bf63→f4a9e56, UNPUSHED —
> <user> pushes & runtime-tests; all built clang-cl + deployed to the offline dir).** Done this session:
> **#1** reachable-DummyAsset recovery (EntityID@part+0x60). **#10** verbose log gated on diag_loot_pos.
> **#2** EMEVD posless lots — investigated + PROVEN unplaceable (~0 recoverable; docs only), CLOSED.
> **★ NEW FEATURE `loot_collectibles`** — runtime AEG gather collectibles from the mod's MSBs +
> AssetEnvironmentGeometryParam.pickUpItemLotParamId(@+0xb8)→ItemLotParam→goods, fully runtime/no-bake.
> See [[aeg-collectible-source]] for the full chain + all the fixes (sentinel -1, per-placement, live
> category fallback @goodsType+0x3e, rune/ember skip, scope to _8xx). Runtime-validated: ~2400 ERR
> collectibles, 0 unclassified. **Also:** ImGui Unicode font fix (œ→'?'), see [[imgui-unicode-font]].
> **OPEN next session:** (a) ✅ DONE — tooltip names for runtime fallback items (gated the existing
> liveLootLabels full-preload on disk sources too; see [[aeg-collectible-source]]);
> (b) ✅ DONE — collectible density filter: NO new code, the existing per-category show_* toggles
> already filter collectibles (markers route into g_buckets[cat] gated by category_visible, and
> classify_item_live maps goodsType→category). Refined the catch-all (8→EquipSpirits, 5/17→MagicSorceries,
> 16/18→MagicIncantations) + documented in loot_collectibles INI help; commit 10a473a. See
> [[aeg-collectible-source]]; (c) ✅ DONE+VALIDATED — ImGui perf: deferred per-marker tooltip
> label (was N FMG/UTF8 string-builds/frame) + finer render bench; commit 8b7560d. Runtime-proven NOT a
> bottleneck (render.worldmap.markers 1.48ms avg @ 4865 drawn, worst case; 50fps = GPU-bound 1650Ti, not
> us). No pass2 needed. See [[imgui-unicode-font]]; (d) ✅ DONE — embedded DejaVu Sans (European)
> via AddFontFromMemoryCompressedTTF, dropped the C:\Windows\Fonts dep; commit e16ab63, see [[imgui-unicode-font]];
> (e) ✅ DONE — "spots restants" census: no native counter exists (verdict after RE-mapping the 3
> collected stores), fixed as a census POLICY = exclude respawning flag-less nodes from the completion
> tally; commit 7601e79, see [[aeg-collectible-source]]. (Inventory-quantity counter via GameDataMan is a
> SEPARATE possible feature, not what #5 was.);
> (f) the ARCHITECTURE-DEBT clean-up (single curated source vs the 4 overlapping loot sources — see
> [[aeg-collectible-source]]). All optional/independent; the feature works as-is.

> **2026-06-24 STATUS: feature DONE + runtime-validated on ALL 4 profiles.** Disk-MSB loot
> (config `loot_from_disk_msb` + `loot_msb_dir`) wired, DummyAsset-filtered, Oodle/KRAK-enabled,
> pre-built at init. In-game via the bundled ME3 (`internals/modengine/bin/win64/me3.exe launch -p
> <profile>.me3`; configs staged in `<windows_downloads>\mfg_test\<profile>\`): **0 KRAK skipped on ERR/
> ERTE/Convergence/Vanilla** — Vanilla 0→full (949 maps). Non-err DLLs build via
> `tools/gen_nonerr_stubs.py` (empty ERR-only tables). **12 commits on feat/msbe-disk-parser
> (6f62ed7→38fce39), UNPUSHED — <user> pushes.** See `docs/loot_from_disk_test_status.md` +
> [[msbe-dummyasset-filter]] + [[build-toolchain-clang-xwin]].
>
> **⏳ AWAITING: Linux agent.** Wrote `docs/linux_test_prompt.md` (self-contained brief: native
> offline parser/Oodle + cross-build the Windows DLLs on Linux via clang-cl/xwin + Proton/ME3
> runtime; leads with "rebuild+deploy"). When its results land, fill the **Linux** column of the
> test-status doc (◑→✅). The 4 per-profile ME3 setups are staged in `<windows_downloads>\mfg_test\<profile>\`
> (DLL+INI+.me3); launch via `internals/modengine/bin/win64/me3.exe launch -p <profile>.me3`.
>
> **Open follow-ups (optional/future):** Convergence 489 unclassified (items missing from its
> ITEM_ICONS → fall back to bake), the 21 false-positives (override offset-free + type-10 +
> position-clip), recover-later EntityID recovery (3 lots → true zero-baking trésor), EMEVD positions.

> **★ 2026-06-24 LATER — branch `feat/map-open-probe` (5 commits, UNPUSHED; separate from
> feat/msbe-disk-parser): CreateFileW map-dir fallback + worker-thread build + F1 red-error.**
> Map-dir resolution is now **3-tier** (`loot_disk.cpp`, state machine `DiskLootState`):
> (1) **ancestor-walk** at init (unchanged; the in-package ERR/ME3 layout — DLL at `<ERR>/dll/offline/`
> walks up + probes `mod/map/MapStudio` → ERR's maps; this is why the feature already worked);
> (2) if empty → **Searching**: the **CreateFileW observer** (`loot_open_probe.cpp`, hooks
> `kernel32!CreateFileW`, armed when `loot_from_disk_msb || diag_map_opens`) feeds `on_map_opened_path()`
> the real resolved path of the first `*.msb.dcx` the game opens → **Found**, loader-agnostic (works
> under Wine/Proton too — same PE import); (3) no map within a **120s** in-game timeout → **Failed** →
> the F1 panel shows a **red "maps introuvables" error and builds NO markers** (hard-fail: the disk
> source is REQUIRED when the feature is on; Failed is recoverable if a map opens later).
> **Build moved to a detached WORKER thread** (`map_entry_layer.cpp kick_disk_build`): no render/init
> hitch (was a ~0.7s burst parsing 651 MSBs). markers()/census gate on `g_disk_built` (acquire/release)
> → return EMPTY until built (g_buckets immutable once Found). Discovery kicks the worker IMMEDIATELY
> via a build-trigger callback (was a ~7s wait for the next overlay tick). **Thread-safety:** the worker
> runs concurrent with render-thread loot callers (find_nearby_overworld/messages) → the 3 LotReader
> lazy-inits (`resolve_loot_flag`/`resolve_loot_item_textid`/diag in goblin_inject.cpp) were `if(!s_init)`
> RACES → converted to **std::call_once** (LotReader::row read-only, encode_live_item pure, build_live_bosses
> build-only). New config `diag_map_opens` = verbose `[MAPOPEN]` log. **Test affordance:**
> `loot_msb_dir = __test_fallback__` forces the ancestor-walk empty to exercise tiers 2+3 on a normal install.
> **Runtime-validated (ALL tiers):** Tier1 no-regression (651/3235 identical), Tier2 discovery →
> identical results, **Tier3 red error CONFIRMED in F1** (screenshot), worker (build overlaps map
> streaming, no hitch), thread-safety (no crash). Both PRs MERGED to master (PR #12). Follow-up branch
> `fix/f1-error-overlay` (1 commit, unpushed): F1 header em-dash→ASCII (ImGui Latin-1 font rendered
> `—` as `?`) + a 2nd test sentinel `loot_msb_dir = __test_error__` (forces Failed + suppresses
> discovery — the 120s timeout is NEVER reached in practice because ERR opens a map within seconds,
> so __test_fallback__ always recovers to Found; __test_error__ is the only way to see Tier3). **Why this fallback, not the `system`-alias anchor:** see [[virtualfs-alias-modroot-anchor]]
> — under external ME3, `system` = vanilla install (Option A = REGRESSION); only observing the real opened
> path (CreateFileW) is loader-agnostic. The [MAPOPEN] essai proved ME3 opens each map by CreateFileW with
> the mod path. <user> pushes.

> **★ NO-BAKE BLOCKERS — audited 2026-06-24 (none done this session; the fallback/worker work above
> is ORTHOGONAL, it didn't touch these).** Status vs the 12-item checklist:
> **DONE:** #12 push/PR (PRs #12+#13 merged to master). **PARTIAL/decided:** #8 KRAK — Oodle decompress
> ✅ (resolve_oodle, 0 skipped on LOOSE maps) but **non-loose source (packed BHD/BDT or resident RAM)
> ❌** (no archive code → a clean packed/no-UXM install unsupported); #3 enemy drops = baked-by-default
> (code keeps lotType==2, no separate runtime path); #9 resident route = parser `parse_msb(...,bool
> resident,...)` flag EXISTS but is NEVER called live (stubbed).
> **✅ #1 + #10 DONE 2026-06-24 (branch feat/msbe-entity-recover-dummy, commit a80bf63, UNPUSHED).**
> #1: EntityID offset PINNED = entity sub-struct ptr @ PART entry **+0x60** (eio base); EntityID@sub+0x00,
> EntityGroupIDs[8]@sub+0x1c. msbe_parser reads entityId/entityGroup; loot_disk KEEPS type-9 iff
> entity-bound. Recovered 4910 + 15000990 (now disk-emitted, bake-independent); 2046460000 is entity-LESS
> in the MSB (EntityID 0 + groups 0, also in unreachable list) → NOT recoverable here (recover-later 3→1).
> Validated all ERR maps: 0 new false positives (178→21 holds). #10: verbose per-lot [LOOTDISK] dump now
> gated behind diag_loot_pos. Build OK (clang-cl) + deployed. See [[msbe-dummyasset-filter]].
> **✅ #2 ANALYZED 2026-06-24 (commit 7131018, low payoff — NOT worth a pipeline; not a blocker for #11):**
> measured coverage of the 748 posless lots = 318 ERR-custom (locationless) + 430 non-custom, of which
> only **22 are MSB Treasures #1 already emits** + ~50 EMEVD (47 low-confidence `lot==entity` byte
> collisions, 3 reliable coarg) → **~69 recoverable, 361 are real items with no world anchor** (scripted
> grants / 149 set-piece duplicates sharing a flag / orphan param rows). Existing `scan_emevd_awards.py`
> + `enrich_fallback_with_emevd.py` resolved 0/748 (enemy-drops only). Findings doc
> `docs/re/windows_emevd_posless_positions_re_findings.md`; probes `tools/resolve_emevd_positions.py` +
> `check_posless_in_msb.py` + `check_posless_sf.py`. **PROVEN unplaceable (commit 9b9a8d0):** traced
> EMEVD common template **90005774** — the `lot==entity` collisions are flag-triggered grants
> (`AwardItemsIncludingClients(lot)` on a trigger flag), NOT positions → force-placing = phantom markers.
> Real recoverable beyond #1 = 3 reliable coarg; ~725/748 have no world position by nature.
> **VERDICT: #2 CLOSED — posless lots genuinely unplaceable, do not fabricate markers; not a blocker for #11.**
> **NOT done (the real blockers):** #2 CLOSED (see above; was EMEVD positions)
> (~430, separate RE workstream — partIndex<0 → stays baked). #4 override last-wins (no logic in parser).
> #5 type-10 craft filter (m21_02, ~10 lots). #6 position-clip baked list (~7). #7 multi-lot treasures
> (parser reads ONE itemLotId@td+0x10, line 92). #10 log verbosity — the detailed [LOOTDISK] disk-only +
> RECOVER-LATER dump is gated on `lootFromDiskMsb` (feature flag), NOT `diag_loot_pos` → still info-level.
> #11 the FINAL switch (drop the treasure slice from the committed `data/items_database.json` 18MB +
> loot_lot_linkage.json) — blocked on #2 (and the residual 2046460000). **Highest-leverage next:**
> #2 (EMEVD, also recovers 2046460000) then #11 (final). See the test-status doc
> `docs/loot_from_disk_test_status.md` for the full per-item rationale.

**Goal:** one DLL that derives loot markers from the **active mod's real MSB files** (ERR/ERTE/Convergence/
Vanilla — whatever the game mounted), instead of a committed `data/items_database.json`. Started/advanced
2026-06-24. See [[runtime-msb-resident-plan]] (parser spec), [[fieldins-pool-registry-re]] (why runtime
gimmicks were a dead end), [[ghidra-re-tooling]] (the Ghidra tools).

## ✅ DONE (proven + committed)
- **RE complete + validated:** MSB `Events.Treasure(type==4)` → `{itemLotId@td+0x10, partIndex@td+0x08}` →
  `PARTS[idx]` → `{name@+0x00, position vec3@+0x20}`. Mapped live (resident) AND from disk; exact match to
  the bake. Pre-open (it's the static placement, resident the moment a tile streams). Docs:
  `windows_resident_msbe_layout_re_findings.md` (the exact layout = parser spec),
  `windows_runtime_msb_resident_re_findings.md`, `windows_map_streaming_structures_re_findings.md`.
- **C++ parser EXISTS:** `src/worldmap/msbe_parser.{cpp,hpp}` (+ `tools/msbe_test`), commit **381c88f**
  (other/Windows agent). Disk route, DCX_DFLT via stb zlib (no new dep), disk/resident offset-base flag.
  Reviewed = correct.
- **Oodle:** verifier (commit **25ca0dc**) proves KRAK maps decompress via the game's `oo2core` (mod already
  hooks `OodleLZ_Decompress`, `g_oodle_orig` / GetProcAddress). ERR repacks only MODIFIED maps as DFLT and
  ALL its treasure is in those → **ERR loot needs only zlib, no Oodle**.
- **Transform link VALIDATED (commit 1145ff6):** bake `x/y/z` = RAW MSB block-local (`Part+0x20`);
  `extract_markers.py` applies `world = gridXNo·256 + pos` (+ `WorldMapLegacyConvParam` legacy-conv)
  DOWNSTREAM → **runtime reuses the existing transform, no new RE.** Diff over ALL ERR `.msb.dcx`, LOD0 only:
  **MATCH 3374 (99.3%)**, residual 24 = lots shared across parts (not bugs). Doc
  `windows_msbe_position_transform_validation.md`. Offline diff: `<ghidra_scripts>\msbe_diff{all,00}.py`.

## ⚠️ CAVEATS / MISSING (the work left)
| item | status / note |
|---|---|
| **Vanilla & non-ERR-DFLT positions** | ✅ **KRAK/Oodle DONE (commit fb2e289).** `dcx_decompress` now takes an OodleDecompressFn; `loot_disk::resolve_oodle()` grabs oo2core_6_win64.dll!OodleLZ_Decompress (in-process, GetModuleHandle) and decompresses KRAK (single call, threadPhase=3). Validated offline via oo2core ctypes over all 4 profiles' map/MapStudio: **100% of _00 maps parse** — ERR 651, ERTE 458, Convergence 468, Vanilla 949 (Vanilla 0→3193 asset-lots). NOTE: this reads the LOOSE UXM-unpacked / mod-overlay map dirs (loot_msb_dir). Reading maps still PACKED in Data .bdt archives (a clean vanilla install, no UXM) is a separate concern — not needed here since the box is UXM-unpacked + mods stage loose maps. |
| **748 EMEVD-granted lots** (prompt 607cb65) | no MSB position (x=y=z=0, partName=""). ~318 ERR-custom likely location-less by design; ~430 recoverable via EMEVD→entity/region→pos. **Separate RE workstream**, low priority (2.4% of bake, half unrecoverable). |
| **Enemy drops** | ⚠️ 25608 = the RAW offline enemy-lot universe (ItemLotParam_enemy / all enemy placements), NOT baked markers. Actual baked enemy MARKERS = **119** (LootSource::Enemy, ERR provenance regen 2026-06-25). On the enemy `ChrIns`, NOT MSB Treasure → out of MSB-parser scope; stay baked or a separate runtime path. SMALL slice, not dominant. |
| **Multi-lot treasures** | parser reads ONE `itemLotId@td+0x10`; ER Treasure can have extra lot slots after → minor under-read; reconcile during diff. |
| **partIndex == 0xFFFFFFFF** (~14–16) | item-glow / no physical part → no MSB position (→ EMEVD/region). |
| **373 MSB lots not in the bake** | possibly ERR-added loot the bake missed (the runtime ADVANTAGE) OR lots filed under enemy/emevd. Characterize during wiring. |
| **resident=true route** | parser flag exists but STUBBED / not live-tested; only the disk route is exercised. |
| **WIRING DONE (2026-06-24, branch feat/msbe-disk-parser, uncommitted)** | disk parser now feeds the DLL marker pipeline behind config `loot_from_disk_msb` (default off) + `loot_msb_dir` (empty=auto: DLL mod folder → game dir). New `src/worldmap/loot_disk.{hpp,cpp}` (file discovery `m*_00.msb.dcx`, DCX_DFLT decompress, parse, filename→area/gx/gz, pos=Part+0x20 X/Z). `map_entry_layer.cpp build_buckets`: builds disk loot via the SAME `push_marker` (reuses `marker_world_pos` transform + live `resolve_loot_item_textid`/`resolve_loot_flag`); item→Category via NEW `goblin::item_marker_category(key)` (ITEM_ICONS.category col). **lotId-coverage REPLACE**: baked rows with lotType==1 & lotId placed by disk are dropped; EMEVD/enemy stay baked. `msbe_parser.{cpp,hpp}` + `loot_disk.*` ADDED to CMakeLists (were tools-only before). **Built OK** (clang-cl/xwin, MapForGoblins.dll 4.91MB). Logs `[LOOTDISK]` (dir, parsed count, positioned treasures, KRAK skipped, emitted/covered/unclassified, replaced). **TODO: in-game diff** (enable flag, point loot_msb_dir at ModEngine2 mod's map\MapStudio if auto picks wrong, compare treasure positions vs bake). |

## Migration method (agreed)
Keep the bake as the **validation oracle**, migrate the **TREASURE slice (4075) first**, keep enemy(119 markers; 25608=raw lot universe, NOT markers)+
emevd(711) baked. **Wiring rules (from the diff):** parse **`_00` maps only** (skip `_01/_02/_99` connect/LOD
proxies); position = `Part+0x20`; grid/area from the filename; a lotId → **MULTIPLE parts, emit each**. Diff
runtime vs bake per map; flip the source only when the treasure slice matches.

## ✅ DUMMYASSET FILTER (2026-06-24, in-game diff resolved the disk-only puzzle)
In-game diff showed ~178 disk-only lots (in disk, not in bake's lotType==1 slice), mostly goods 2020001
(crafting mat) in m20/m21/DLC. Root cause: the offline pipeline (`extract_all_items.py:390-516`) excludes
**312 "unreachable_only_lots"** via 4 criteria: (1) Asset GameEditionDisable=1, (2) overridden treasure
(same part, last lot wins), (3) **dummy-only lot bound to a DummyAsset with zero EntityID+groups**,
(4) position-clip vs vanilla (`unreachable.py`, only 13 manual, needs vanilla cache → NOT runtime-derivable).
**RE WIN (validated offline over all 487 DFLT maps):** MSBE PART entry **+0x0c = part-type enum: 13=Asset,
9=DummyAsset**. 305/312 unreachable = type 9. So the parser now reads `partType` (msbe_parser) and the disk
layer **drops DummyAsset (type 9)** placements — explains 97.8% of exclusions with ONE field read, no
EntityID offset, no baked list. The 13 "reachable_dummy" (type9 WITH entityID, bake keeps) gracefully FALL
BACK to their identical baked marker (coverage-replace keeps uncovered rows) → zero loss. Residual = 12
type-13 unreachable (GED/override/clip) — tiny; override is offset-free if needed, clip needs vanilla.
**CONFIRMED in-game: disk-only collapsed 178→21 (0.6%). Committed b8b2911** (after 6f62ed7 wiring). The
21 residual = curation long-tail: position-clips (Golden Seeds m60_42_51/43_52 under-wall, Altus node
m60_49_40 — need vanilla geometry), a **type-10** crafting subtype (m21_02 AEG463_610 block, goods
2020001), and lots in items_database.json but NOT loot_lot_linkage.json (bake extracted but deliberately
unlinked). Benign over-display, no missing loot. 21 residual ACCEPTED for now (override offset-free + position-clip
tiny baked list = optional future polish). **Recover-later confirmed = 3** (4910 m12, 15000990 m15,
2046460000 m61-DLC) — logged at runtime, doc'd. **#3 THREADING DONE (commit 17fdc84):** build_buckets
now pre-built at setup_mod (init thread, after params ready) via std::call_once(ensure_buckets) — kills
the ~0.5s first-map-open hitch; lazy markers()/census still no-op through call_once. Full commit chain:
6f62ed7 wiring, b8b2911 DummyAsset filter, 7aaf60d recover-later log+doc, 40ecb3c recover-later fix,
451f018 doc fix, 17fdc84 prebuild threading. All on feat/msbe-disk-parser, UNPUSHED (<user> pushes).

## ▶ NEXT STEPS (in order)
1. ✅ **Wiring DONE** (2026-06-24, see the CAVEATS table row). Remaining for this step: **in-game diff** —
   enable `loot_from_disk_msb`, set `loot_msb_dir` to the ModEngine2 mod's `map\MapStudio` if auto-detect
   misses, read the `[LOOTDISK]` log, and compare runtime treasure positions vs the bake. Then commit (await
   <user>'s "go"). EMEVD follow-ups he raised: (a) recover EMEVD lot positions via RE — **check the VANILLA
   ER files first to confirm those entity/region defs exist**; (b) caveat to flag — a mod granting loot via
   BOTH EMEVD events AND regulation.bin ItemLotParam: the lotId-coverage replace handles it (disk wins where
   it has an MSB part; identity is always read live from the regulation's ItemLotParam, so regulation-only
   placements are unaffected and EMEVD-only lots stay baked).
2. **Vanilla/KRAK support** (when targeting non-ERR profiles): wire the Oodle path + a map source for
   non-loose maps (resident MSBs in RAM, or game Data archive read).
3. **EMEVD positions** (607cb65): recover the ~430 recoverable EMEVD-granted lot positions.

## READ FIRST next session
Docs (in `docs/re/`): `windows_resident_msbe_layout_re_findings.md`, `windows_msbe_position_transform_validation.md`,
`windows_runtime_msb_resident_re_findings.md`. Commits: 381c88f (parser), 25ca0dc (Oodle), 607cb65 (EMEVD
prompt), cf57e61 / 822fcd7 (layout+disk), 1145ff6 (transform). Branch `feat/msbe-disk-parser`.
