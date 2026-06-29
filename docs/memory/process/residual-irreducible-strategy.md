---
name: residual-irreducible-strategy
description: How to think about "accepted/irreducible" no-bake residuals — they are an OPEN question (disk-parse gap vs unknown runtime path), not a closed verdict. Classify before accepting.
metadata:
  node_type: memory
  type: feedback
---

**An "ACCEPTED / irreducible" no-bake residual is almost never PROVABLY irreducible — it's a
provisional verdict that really means one of two OPEN problems we haven't cracked yet.** <user>'s
framing (2026-06-26): when a baked row survives every disk pass, the honest question isn't "is this
content irreducible?" but **"which of these two are we hitting?"**

- **(A) DISK-PARSE GAP — the data IS in the mod's real files, we just don't parse the structure (yet).**
  The position/identity/flag lives in an `.msb`/`.emevd`/regulation structure the runtime parser doesn't
  read cleanly. The bake got it via an OFFLINE HEURISTIC (id-prefix/byte-match, curated template list)
  that the disk parser can't reproduce. Proven instances:
  - **29 "unknown" chests** — EMEVD-scripted via MAP-SPECIFIC per-tile templates (1034432261, 31082770…)
    with inconsistent (entity,lot) layouts. The data is RIGHT THERE in event/*.emevd, but no clean
    structural signature isolates the 12 m60 chests from the ~383 other m60 scripted awards already
    covered (the callee≥1e9/entity@2/lot@(n-2) rule matched 395). Recovering them needs RE of the chest-
    event CONVENTION (what marks an event as a "notable chest award"), not a heuristic. [[nobake-coverage-scoreboard]]
  - **16 treasure debake-gap + the AEG099_630 corpse loot** — the armour-set lot is delivered by a corpse
    asset's `pickUpItemLotParamId`, but on a model OUTSIDE the `_8xx` collectible scope, and the offline
    bound it lot→asset by a byte-match HEURISTIC, not a clean MSB Events.Treasure structure. Data in MSB,
    parser can't see the binding. [[aeg-collectible-source]]
  - **35 enemy "mislabel"** — orphan ItemLotParam_enemy lots referenced by NO NpcParam (in ERR, vanilla,
    full table). The bake tagged them Enemy from an older regulation; the item appears elsewhere OR via a
    scripted/corpse path we don't parse. [[msbe-enemy-loot-offsets]]
  - **★ 9 Reforged Rune Pieces — CRACKED 2026-06-26 (the cleanest category-A yet: bake reads an arg the
    runtime skipped).** "8 overworld + 1 cross-tile, flag != EntityID" had been ACCEPTED as a different-
    convention residual. The lens reopened it: the bake's extractor (extract_all_items.py:651) reads the
    EXPLICIT bossEntity@16 of the 90005860/61/80 init; the runtime (msbe_parser.cpp:653) read only
    (defeatFlag@8, baseLot@24) and used defeatFlag-as-entity (a convention shortcut) → failed for the 9
    where defeatFlag != EntityID. FIX = parse @16 + position fallback. tools/_probe_boss_piece_entity.py
    proved 9/9. baked 37→28. **GENERAL LESSON: when the BAKE has a residual positioned but the RUNTIME
    leaves it baked, diff the offline extractor vs the runtime parser ARG-BY-ARG — the gap is often an arg
    offset the runtime skipped, not irreducible data.** [[nobake-coverage-scoreboard]]

- **(B) UNKNOWN RUNTIME PATH — there's a LIVE game structure/mechanism we haven't RE'd.** The position
  and/or completion-state exists in memory at runtime but we don't know the manager/offset chain. Proven:
  - **Kindling Spirits 5 — ASSUMED category-B, PROVEN category-A 2026-06-26 (the lens working as intended).**
    We'd parked it as B ("live state heap-only, no position"). But the B blocker was about the *state*; the
    *position* (what's actually missing to place the marker) is trivially ON DISK: tools/_probe_kindling_region.py
    found the 5 as MSB **SFX regions** in m60_45_37_00.msb named `KindlingSpirit_0001..0005`, eids
    1045373501..505, WITH positions (e.g. 0001 = -27.7,17.0,56.1). (Also a `KindlingSpiritX_*` set 511..515 =
    likely the lit/activated variant.) → **category-A, CHEAP fix — ✅ SHIPPED + RUNTIME-VALIDATED 2026-06-26** (branch feat/emevd-treasure-dup-debake,
    build 10:27): region parser now keeps SFX regions named `KindlingSpirit_` (excludes the lit-variant
    `KindlingSpiritX_`); the baked loop OVERRIDES each baked kindling marker's POSITION from the disk region of
    the same name (matched on object_name) + flips source→DiskMSB, KEEPING the baked row_id/object_name so
    goblin_kindling's row_id-keyed graying + its MAP_ENTRIES slot table are untouched. `re-sourced 5`, `Kindling
    baked=0 disk=5`, census flagged 5/5, baked 118→113, **🔴 baked-only count 1→0** (no more baked-only categories).
    LESSON: a "B" verdict on STATE doesn't make POSITION a B problem — check disk for the position FIRST
    (cheapest). And when state-tracking is bake-coupled (row_id slot table), OVERRIDE the baked marker's position
    + re-tag source rather than category-wipe + re-emit — keeps the coupling intact, still counts as off-bake.
  - **Quest NPC state** — tracked in ESD state machines + scattered flags; no queryable per-NPC structure
    found (RETIRED to the hand-authored Quest Browser). [[nobake-coverage-scoreboard]]
  - **The loaded-loot walker** (MapIns+0x460) only sees SPAWNED/opened loot, not resident sealed chests —
    a known runtime-scope limit, not a true dead end. [[fieldins-pool-registry-re]]

**HOW TO APPLY (before stamping ACCEPT on any residual):**
1. State WHICH category (A or B) and WHAT SPECIFICALLY is missing — name the MSB/EMEVD structure we can't
   parse, or the live manager we haven't RE'd. "Irreducible" with no named blocker = analysis not finished.
2. "ACCEPT" should mean **"ROI too low to chase RIGHT NOW"**, never "provably impossible." Record the
   blocker so a future session can reopen it when the ROI changes (e.g. a tool/RE lands that cracks it).
3. Watch for the recurring trap: a verdict that turns out to be a dedup-KEY bug or a wrong-offset, not real
   residual (Material Nodes, Rune/Ember pieces, the ammo mis-bucket all "looked irreducible" then weren't —
   [[nobake-coverage-scoreboard]]). A small 🔴/residual on an OTHERWISE-covered category ⇒ suspect a
   mechanism bug first, content-irreducibility last.
4. The two categories suggest different NEXT MOVES: (A) → extend the disk parser / find the structural
   signature (offline-provable with the probes); (B) → live RE (RPM, vft scan, Ghidra) to find the manager.

**★ LOOT RESIDUAL NOW PROVEN IRREDUCIBLE AT MECHANISM LEVEL 2026-06-26 (<user>: "on a jamais vérifié leur
nom/provenance").** Verified the 80 baked-loot residual BY ITEM NAME + recovery path, not assumption:
- tools/_probe_residual_names.py resolves each [RESIDUAL-ROW] lot → FMG item name + lot table + NpcParam ref.
  The residual is REAL named items (Bloodfiend's Arm, Carian Knight's Sword, Ghost Gloveworts, Golden Runes…),
  not faceless phantoms — so "accepted" deserved the by-name check.
- tools/_probe_unknown_chests.py run over ALL 80 (asset-pickup vs EMEVD bank-2000): **0 asset-pickup · 29
  emevd-only · 51 neither**. → 0 corpse-asset pickUpItemLotParamId path exists (hypothesis REFUTED).
- tools/_probe_treasure_part.py: the **16 treasure-src lots have ZERO MSB Treasure event** → they are bake
  FALLBACK records (extract_all_items.py:1086 "unmatched ItemLotParam_map") placed at **(0,0,0)** = tile-corner,
  NO real position to read. The 35 enemy = orphan ItemLotParam_enemy, no NpcParam AND no bank-2000 EMEVD. The 29
  unknown = per-tile chest templates (1034432261/90005555…), indistinguishable from the 383 covered.
- **CONCLUSION: no RECOVERY lever (no world placement to read), BUT a big CLEANUP lever found.** <user>
  in-game-tested: Lordsworn's Greatsword NOT findable at Agheel Lake North → "le bake pointe au mauvais endroit."
  **★★ ROOT CAUSE = MERCHANT PHANTOMS. The bake's unmatched-ItemLotParam fallback (extract_all_items.py:1086)
  invents a marker at the tile corner (0,0,0) for items with NO world placement that ARE sold by a merchant.**
  ERR's shop sells most weapons/gloveworts (ShopLineupParam, sellQuantity=-1 infinite). Proven: tools/
  _probe_find_item (item 3030000 → 1 lot, placed nowhere; ShopLineupParam sells it ∞) + _probe_resid_shop
  (**42/80 residual are shop-∞**: Armaments 21, Gloveworts 11, Talismans 3, Armour 2, +5). The 8 finite-shop +
  30 not-in-shop (Golden Runes, emevd chests) are NOT phantoms, kept.
  **★★ SHIPPED — feat(nobake) commit e1ffbdd, branch feat/world-snow-statues-lod, config drop_merchant_phantoms
  (default on), commit 82b5cf8, RUNTIME-VALIDATED 2026-06-26 build 11:54 (dropped exactly 42 = offline
  prediction, baked 92→50, Armaments/Gloveworts→0, 6 cats flipped 🟢, no false positives).** Reads ShopLineupParam LIVE (RawShopRow b[0x34];
  equipId@+0x00 s32 / sellQuantity@+0x14 s16 / equipType@+0x17 u8, pinned via paramdef walk + GetRowSize()==52,
  tools/_probe_shop_offsets.py). build_shop_infinite_keys() → set of encode_live_item keys (equipType→cat 0→2/1→3/
  2→4/3→1); at the baked-residual loop (map_entry_layer.cpp), drop any baked loot row whose resolve_loot_item_textid
  key ∈ set AND reached the loop (no disk twin). Expected drop ~42, baked 92→~50 ([MERCHANT-PHANTOM] log under
  diag_loot_pos, [LOOTDISK] dropped N summary). LESSON: a residual at block-local (0,0,0) with no MSB/EMEVD source +
  the item sold ∞ in a shop = bake fallback phantom, DROP (not a position gap to recover). Generalizes to any mod's
  merchant. NEXT after validation: the 30 not-in-shop (Golden Runes 23 + emevd chests) — still genuine residual.

**★★ AUTOMATED RECOVERY ORACLE 2026-06-26 (tools/_probe_residual_recover.py) — the lens, scripted.**
<user>: "creer un script probe automatisé pour verifié partout si les autres sont pas recuperable."
ONE sweep over every [RESIDUAL-ROW] lot cross-references it against EVERY known disk placement mechanism
(shop-∞ / AEG pickUpItemLotParamId / MSB Treasure event / NpcParam-enemy / EMEVD template-bound vs loose
entity / boss-chain) → per-lot verdict + recovery-candidate list. Reports entity KIND (enemy/asset) because
that decides which runtime pass can reach the award (the emevd direct pass joins to ENEMIES only). **Calibrated:
its 16 IRREDUCIBLE verdicts MATCH ground truth** (orphan-enemy Golden Runes + finite-shop + m35 (0,0,0) fallback).
Run it after any [RESIDUAL-ROW] dump to re-triage in one shot instead of per-category by hand.
**Findings on the current 25 loot residual (post-entity16): 16 IRREDUCIBLE + 9 CANDIDATES (position IS on disk):**
- **✅ SHIPPED — RECOVER-TREASURE: Dragonwound Grease lot 1040540050 (merge c666c00, baked 28→27).** 0 Treasure
  rows in _00 m60_40_54, but 1 in the LOD tile m60_10_13_02 w/ cross-tile part `m60_40_54_00-AEG099_620_9000`.
  New `load_lod_treasures()` (loot_disk.cpp, treasure analogue of load_lod_feature_assets) scans non-_00 tiers for
  cross-tile-prefixed Treasures; the baked-residual loop RE-SOURCES an uncovered (∉disk_lots) row's position from
  the LOD part + flips source→DiskMSB (KINDLING PATTERN — no new marker, so the 15 sibling-covered LOD lots are
  never doubled). RUNTIME-VALIDATED build 13:09: LOD scan 19, re-sourced 1, Loot - Greases baked=0 🟢, TOTAL 28→27,
  zero regression. KEY GOTCHA: the naive "skip if a _00 base within ±50" guard was WRONG (it skipped 1040540050,
  which IS uncovered — the sibling-walk stops at an ItemLotParam chain gap); the only correct "covered" signal is
  the runtime disk_lots set, hence the kindling re-source (driven by the residual loop) not an emit-with-guard.
- **1 EMEVD-BOUND-ASSET: Blaidd's Half-Wolf spirit ash (lot 1034500110).** T90005750 binds lot↔entity
  1034501740 but that entity is an ASSET (AEG099_090 seal), not an enemy → the runtime enemy-join can't reach
  it (nor does the bake's enemy-only join — Blaidd is baked via items_database). Needs an emevd→ASSET join
  (broader lever); ROI low (1 NPC-reward marker).
- **7 EMEVD-LOOSE? (verify offsets before trusting):** Taylew (T90005555), Radagon's Scarseal (T90005880),
  Viridian Amber Medallion +1 (T90005300), 2 m60 Larval Tears (per-tile t1047372200/t1049552400), Golden
  Rune[200] m12 + Somber Scadushard m20 (per-tile t90005500/501). Entity co-occurs in the init blob but is NOT
  at the template's bound lot/entity offsets (several anchor to ASSETS) → each needs a per-lot offset RE like
  the boss pieces did before claiming recoverable. These are the old "29 unknown chests / accepted" — the
  oracle says they're NOT a uniform dead-end; at least the position exists on disk for each.
**LESSON: "29 unknown chests accepted" was too coarse — the oracle splits it into 16 truly-positionless vs 9
with a readable disk anchor. Re-run the oracle, don't re-accept blind.**

**Why:** we've twice over-declared "irreducible" and been wrong (the 328→16 treasure debake-gap; the 35
enemy "phantoms" that hid a real scripted-boss-reward lever). Treating accept as a closed verdict stops the
investigation prematurely; treating it as a named open blocker keeps the door open without burning ROI now.
See [[nobake-coverage-scoreboard]] (the per-category residual map), [[world-feature-msb-identities]]
(the disk-side identities for category-A world features), [[runtime-msb-resident-plan]] (the runtime-path side).
