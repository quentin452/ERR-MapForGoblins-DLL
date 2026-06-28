---
name: disk-parser-coverage-gaps
description: "RESUME POINT — real items MISSING from the map because the disk parser re-implements the EMEVD/MSBE format and misses some (NOT a classification bug). Next session: single source of truth for the parser; the 883 unclassified is a SEPARATE deferred bug."
metadata: 
  node_type: memory
  type: project
---

**Found 2026-06-27 (<user> spotted 3 missing-item classes).** Real ERR items that exist in the
data but never get a marker — and **NOT** because of classification (the user's first guess, the 883
`unclassified`, was ruled out for these):

The 3 reported classes + offline ground truth (regulation slot-dump via the `dump_loot_flags.py`
bootstrap: `param_to_dict(read_param(bnd,'ItemLotParam_map'/'_enemy'))`, ERR regulation):
- **Ghostflame Torch** — lot `12070500` (ItemLotParam_**map**), slot-1 = `(24050000, cat2)` weapon → would
  classify **Armaments**. In `m12_07` (= an UNDERGROUND tile). `m12_07.msb` IS parsed (40 treasures, 39
  positioned). MapGenie lists it underground.
- **Burred Bolt** — lots `12070320` (map, m12_07 UG) + `1038490030` (map, m60_38_49 OW), slot-1 =
  `(52040000, cat2)` ammo (id≥50M) → would classify **Ammo**. User: "10x never shown".
- **Larval Tear ×4 + lots of armor** — enemy-table lots `333062001/011/021`, `333065001`
  slot-1=`(8185,cat1)` goods; armor `337000805` slot-1=`(301000,cat3)` protector. "Dropped by bosses,
  completely absent from render." (333062021 + 333065001 are 2 of the 16 [[nobake-coverage-scoreboard]]
  baked residuals — they DO draw, baked, on the UG page; the OTHERS vanish.)

**KEY FINDING: these are NOT `unclassified`.** Every lot resolves to a valid slot-1 item that
`resolve_loot_item_textid` + `item_marker_category` would bucket fine (torch→Armaments, bolt→Ammo,
larval→Key/Quest, armor→Armour). The earlier slot-1-empty and lotType-mismatch hypotheses were both
DISPROVEN by the offline dump. So the blocker is in the disk-pass **reach/scope**, not classification:
- **Boss drops (enemy table)**: `build_disk_enemy_markers` only iterates **placed MSB Enemy parts**.
  Bosses are often EMEVD-spawned / not placed → their `333xxxxx` enemy lots are never reached → they
  stay baked or disappear. The EMEVD pass (`build_disk_emevd_markers`) is supposed to cover scripted
  boss drops but evidently misses these.
- **Treasures (torch/bolt in m12_07)**: parsed but the specific marker may be dropped at
  position/dedup, OR it IS emitted on the UG page (untested — see below).

**<user>'s root-cause theory (the next-session thesis):** the disk parser **re-copies / re-implements
the EMEVD + MSBE binary format** rather than reading ONE authoritative source, so the re-implementation
drifts and misses items the regulation/files actually contain. Cf. [[runtime-msb-resident-plan]]
(C++ MSBE parser) + [[nobake-endgame-roadmap]] Phase 3 (offset-free / format-driven parser) +
[[param-offset-source-of-truth]] (read offsets from the exe, don't hardcode).

**★ ROOT CAUSE CRACKED 2026-06-27 (offline, tools/_probe_missing_lots.py — traces each lot through
MSB Treasure / NpcParam(+placed?) / EMEVD(all banks) / sibling-window in the REAL ERR files).** The
two reported classes are TWO DIFFERENT problems, and NEITHER is EMEVD/MSBE format drift:
- **GROUP A — the boss enemy lots (Larval Tears 333062001/011/021, 333065001; armor 337000805) =
  REAL PARSER GAP, offline-proven, clean fix.** Each target is a **sequence-sibling base+k of an
  ItemLotParam_enemy base that a PLACED enemy's NpcParam references** (333062001 = 333062000+1, NpcParam
  33300662 on c3330_9006 @m12_01; 337000805 = 337000800+5, NpcParam 33700865 on c3370_9102 @m12_02; etc.).
  The base IS reached by the enemy pass — but **`build_disk_enemy_markers` (map_entry_layer.cpp:529) has
  NO Mechanism-C sequence-sibling walk**, unlike `build_disk_loot_markers` (:295-339) AND
  `build_disk_emevd_markers` (:789-836) which BOTH walk base+1..base+k. So multi-item enemy/boss drops
  only mark their base lot; the Larval Tear/armor at base+k is silently dropped. → FIX = add the same
  base+k walk to the enemy pass (lotType 2 = ItemLotParam_enemy, gate sflag!=0, stop at the next enemy
  base lot — bases are 10 apart so 333062000+1..+9 captures the tear, stops before 333062010). The
  single-source-of-truth move: factor the sibling walk into ONE shared helper used by all 3 passes (it's
  currently copy-pasted into 2 of 3 — the enemy pass is the omission = the "drift").
- **GROUP B — the map treasures (Ghostflame Torch 12070500, Burred Bolt 12070320 + OW 1038490030) =
  NOT a parser gap.** All three are **MSB Treasure bases bound to LIVE Assets** (AEG099_620_90xx /
  AEG099_610_9003, part=Asset eid=0 → NOT inert DummyAssets the runtime drops). `build_disk_loot_markers`
  PARSES + SHOULD emit them. m12_07 = UNDERGROUND page (likely just unchecked); **the OW one 1038490030
  (m60_38_49) is the real test — if IT is missing it's downstream (finalize dedup / category vis), not
  parsing.** → needs the runtime [WATCH]/[SKIP-ROW] diag (memory plan) to confirm emit vs downstream drop.
  (Aside: 12070320's lot also appears as ev 90005261/90005211 inits — those are pickup-glow/enable
  events, correctly NOT item-award templates; the treasure event is the real source.)

**★ SHIPPED (branch feat/enemy-sibling-walk, built+deployed 2026-06-27, AWAITING <user> runtime-test):**
- **GROUP A fix DONE.** Factored the Mechanism-C walk into ONE shared helper `emit_lot_siblings()`
  (map_entry_layer.cpp, right before build_live_bosses) and wired it into ALL 3 passes — treasure +
  emevd now CALL it (replacing their inline copies), and the **enemy pass gained the walk it never had**
  (table = the base's own lt, stop at the next enemy base `enemy_bases`, skip treasure_lots, independent
  of the base's notability filters so a respawning base whose +1 is a one-time tear still yields it).
  Chain-contiguity verified: `lot_row_in_table` returns false ONLY on an ABSENT row (try_get null);
  existing-but-EMPTY rows return true (sflag=0→continue), so 337000800→+5 traverses the 4 empty rows
  337000801-804 and reaches the armor; 333062000→+1 reaches the tear (gap at +4). build OK (clang-cl),
  DLL deployed to the offline dir.
- **GROUP B diag DONE.** Added a gated [WATCH] per-lot trace (config diag_loot_pos): kWatchLots set
  (the 8 reported lots, editable) → [WATCH] emit-*/skip-* lines at every disk-pass emit/skip site + a
  finalize "[WATCH] lot=X SURVIVED in cat=…/ABSENT" summary. A watched lot with NO line = never reached;
  emitted-then-ABSENT = downstream finalize drop. **NEXT: <user> runs with diag_loot_pos, greps [WATCH]
  — expect the 5 enemy lots now "emit-sibling…SURVIVED" (Group A working); for the 3 treasures, SURVIVED
  ⇒ Group B is just the m12_07 underground page (unchecked) / the OW Burred Bolt 1038490030 is the tell.**
- Tool: **tools/_probe_missing_lots.py** = the offline blocker oracle (trace any lot through MSB Treasure
  /NpcParam(+placed?)/EMEVD(all banks)/sibling-window in the real ERR files). Re-run it for any new report.

**★★ RUNTIME-VALIDATED 2026-06-27 (log run 03:28, new DLL + diag_loot_pos).** [WATCH] confirms ALL 8 lots
SURVIVED in correct cats: the 5 enemy lots via `emit-sibling tbl=2` → Key-Larval Tears ×4 + Equipment-
Armour; the 3 treasures via `emit-treasure` → Armaments + Ammo ×2. **Group A WORKS.** Enemy pass: 123 base
+ **236 sequence-siblings** (0 unclassified, 0 rune/ember FP — no flood). Key-Larval Tears baked=0 disk=23.
**Group B = confirmed NOT a parser gap**: OW Burred Bolt 1038490030 SURVIVED in Loot-Ammo → "never shown"
was category-filter/page perception, not a drop. **★ BONUS: [BAKED-RESIDUAL] TOTAL 16→1** — the enemy
sibling walk recovered ~15 baked rows that were base+k enemy/boss bundles WITH real positions (the Larval
Tears + boss armour + Ashes/Seeds/Reusables the scoreboard had filed IRREDUCIBLE; the "2 finite-shop Larval
Tears" were never finite-shop — reinforces [[residual-irreducible-strategy]]). The lone survivor = 1
positionless Golden Rune(200) lot 35000580 @m35_0_0 (tile-corner fallback). Phase-1 baked→0 nearly reached
as a side effect. No regression (treasure/emevd siblings 313/269 unchanged, semantics-preserved refactor).

**❌ THE "2009 PASS" WAS WRONG — REVERTED 2026-06-27 (b77e8b5). baked stays at 1.** I claimed the lone
Golden Rune (35000580 @m35) was placed by EMEVD `2009[00]` as an item-lot-at-asset. **FALSE: the EMEDF
(tools/er-common.emedf.json) proves `2009[00]` = "Register Ladder"** (args = DisableTopFlag, DisableBottom
Flag, EntityID). The values I read as "item lots" are ladder disable-EVENT-FLAGS that only COINCIDENTALLY
equal ItemLotParam row numbers (FromSoft reuses the m35/m12 id range). Verified: the anchor assets have
`pickUpItemLotParamId=-1` (not pickups); the only position-bearing item instruction `2003:34 Spawn Snuggly`
has 0 calls in ERR; `2003:04/36 Award Item Lot` are positionless gives. So 35000580 + the 3 Siofra goods
have NO world placement — the pass made 4 PHANTOM markers at ladders. **TRUTH: 35000580 is an unplaced/cut
ItemLotParam row the bake mis-positioned at the ladder = a bake phantom (drop it, don't place it).**

**★ LESSON (the important one): before building an EMEVD loot pass, confirm the instruction SEMANTICS with
the EMEDF — match the lot to an arg the EMEDF TYPES as "Item Lot", never infer an award from arg
co-occurrence.** A lot-number appearing in an instruction is usually a flag/entity coincidence. The
reachability census (tools/lot_reachability_census.py, EMEDF-precise) is the guard that caught this and
should be re-run before trusting any new EMEVD pass. Cf. [[nobake-coverage-scoreboard]].

**★ DEUX AXES DE "COVERAGE" — ne pas confondre (clarifié 2026-06-28, à propos du Black Syrup invisible).**
`docs/parser_coverage_results.md` mesure la couverture d'**OCTETS** (le parser lit-il tous les champs utiles d'UN msb + UN emevd ? les unread = padding/headers/chemins SIB) → "correct" = aucun champ sauté dans les structures parsées. Ça NE dit PAS que tous les items sont affichés. L'autre axe = couverture de **SOURCE/MÉCANISME** : certains items n'ont AUCUNE placement monde à parser.
- **Black Syrup (quête de Moore)** = donné par **dialogue NPC (talk/ESD)**, pas de ligne Treasure ni lot Asset au Main Gate Cross. Donc invisible ≠ byte-coverage gap ≠ sibling-walk ≠ EMEVD-award → c'est une **classe de source non modélisée : items donnés par talk/dialogue + récompenses de quête + shop**. Cf. [[map-data-obs-moore-enemies]].
- 2 sous-cas à TRANCHER avant de coder : (1) pur talk/ESD → item SANS position (comme les positionless-irreducible) → matcher MapGenie = **layer éditorial récompense-de-quête / QuestNpcLayer** ([[plan-quests-audit]]), PAS un fix parser ; (2) EMEVD item-award réel → récupérable via le pass EMEVD-award (mais ⚠️ la leçon "2009 pass" ci-dessus : confirmer la SÉMANTIQUE EMEDF avant). MapGenie place éditorialement la récompense au NPC même si c'est un don de dialogue.
- Vérif cheap : F1-search "Black Syrup" (résultat ⇒ existe, problème page/visibilité ; rien ⇒ non émis = source non couverte).

**STILL TODO:**
1. **baked = 1** (honest). The 1 = Golden Rune 35000580, a positionless bake phantom. Phase-1 (baked→0)
   needs a generic "drop unplaced-lot bake phantom" (a lot with NO reachability per the census) — that's
   the real lever, NOT placing it. Defer or do as a small curated/▶generalized drop.
2. **Phase-2** ([[nobake-endgame-roadmap]]): delete goblin_map_data + cut MAP_ENTRIES deps. (baked is 1
   not 0, but the 1 is a phantom, so Phase-2 isn't blocked on real coverage.)
3. **GROUP B** (Ghostflame Torch/Burred Bolt) — those ARE on the map (treasures, Armaments/Ammo), real +
   correct (the enemy fix + Group B are sound; only the 2009 pass was wrong).
4. **The 883 `unclassified` is a SEPARATE deferred bug.** Likely overlaps the census's 1149 `unreferenced`
   (cut/unused rows) — cross-check with docs/lot_reachability.md.

**Cheapest un-blocking step still pending (asked <user>, no answer yet):** search `Ghostflame Torch` /
`Burred Bolt` / `Larval Tear` in F1 — a RESULT ⇒ the marker exists (page/visibility issue, m12_07=UG),
NOTHING ⇒ not emitted (real gap → add a `[SKIP-ROW]` per-item diag at the treasure/enemy/emevd skip
sites, gated by `diag_loot_pos`, to name the exact drop reason; mirror the existing `[BAKED-RESIDUAL-ROW]`
/ `[RESIDUAL-ROW]` feeds in map_entry_layer.cpp). This is the "name the blocker" step from
[[residual-irreducible-strategy]] — do it before accepting any item as irrecoverable.
