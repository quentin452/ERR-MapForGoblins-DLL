# Plan — close the 31 MapGenie category gaps + 2 custom respawn categories

Status: GROUP 1 partially landed (2026-07-01). Research done via subagent (2026-07-01); findings below
are grounded in actual param defs / source citations. RE fully discharged
(`docs/re/windows_mapgenie_category_coverage_re_findings.md`).

## Progress (2026-07-01, branch `feat/mapgenie-group1-landmarks`)

- **DONE — Tier 2(A) landmarks (6 categories).** `WorldDivineTower` / `WorldEvergaol` /
  `WorldMinorErdtree` / `WorldGrandLift` / `WorldDungeon` / `WorldLegacyDungeon` are wired LIVE from
  `WorldMapPointParam.iconId` (`build_live_landmarks` in `map_entry_layer.cpp`, same path as
  `build_live_bosses`). Mod-agnostic, no bake. iconId map + zero-boss-overlap re-verified off-disk
  (`tools/verify_worldmap_iconids.py`). Cross-build (clang-cl+xwin) links clean. All 6 default OFF
  (World section toggles). **In-game verification (ERR + a non-ERR install) still pending (user).**
- **DONE (already shipped) — Ghost = NPC invader = `WorldHostileNPC`.** No new work, per Tier 3.
- **DEFERRED (user, 2026-07-01) — `WorldFarmableEnemy` + `WorldFarmableCollectible`.** These are
  MFG-original (not MapGenie) and each hit a real design fork with no clean answer from the data:
  - `WorldFarmableEnemy`: NpcParam has **no** clean per-enemy "boss" field (`npcType`/`teamType`
    aren't category keys; fog-gated bosses read `disableRespawn==0`), so a `dr==0` pass can't cleanly
    exclude bosses AND would flood the map with thousands of trash placements. Revisit with a curated
    notability/boss datamine (`tools/datamine_enemy_notability.py`) as the non-boss filter.
  - `WorldFarmableCollectible`: farmable MAP gather nodes (`getItemFlagId==0`) already draw under
    their item category; farmable ENEMY drops are dropped at `map_entry_layer.cpp` (the
    `no-one-time-flag` skip). Populating a dedicated bucket is a routing decision (additive vs
    re-route) the user opted to make later, alongside FarmableEnemy.
  The gate mechanics remain correct if/when resumed: `dr==0 ∧ non-boss` (reuse a boss filter) and
  `getItemFlagId==0 ∨ flag_is_repeatable` (both fields already read live — zero new plumbing).

Research done via subagent (2026-07-01), findings below are grounded in
actual param defs / source citations, not guesses — items marked "hypothesis" still need verification.

## Sequencing dependency — do this AFTER `generated_data_removal_plan.md` Phase B

`Category` enum lives in `src/generated/goblin_map_data.hpp:10` and is **duplicated identically across all
4 `generated_*/goblin_map_data.hpp` copies**. Adding 33 new entries means editing 4 files, in lockstep,
until that file is deduped into `generated_shared/` (Phase B of the removal plan). Land that Phase B step
first — it's small and mechanical — so this plan only ever touches one copy of the enum.

## Part A — the 2 custom respawn categories (fully scoped, concrete fields found)

Not MapGenie categories — MapForGoblins-original, distinguishing one-time enemies/pickups from
infinitely-farmable ones.

**(a) `WorldFarmableEnemy`** — enemy that respawns when you rest at a Site of Grace.
- Field: `tools/paramdefs/NpcParam.xml` → `disableRespawn` (u8 bitfield, Japanese label "リスポン禁止か" =
  "respawn prohibited?").
- **VERIFIED 2026-07-01** (`tools/verify_disablerespawn.py`, vanilla+ERR — see
  `docs/re/windows_mapgenie_category_coverage_re_findings.md`). Polarity is only half as clean as the
  original hypothesis:
  - `disableRespawn == 1` → reliably one-time / NOT farmable (invaders, questline NPCs, prop dummies).
    Safe hard short-circuit.
  - `disableRespawn == 0` → **NOT a farmable signal on its own.** The fog-gated main bosses (Rennala
    `20300000`, Draconic Tree Sentinel `32500072`, …) read `0` in BOTH vanilla and ERR — their
    non-respawn is enforced by the boss-defeat event flag + fog gate, not by this field. So `0`
    over-includes bosses.
- `NpcParam` is already read live in `src/goblin_inject.cpp` (existing enemy classification path) — this
  is one additive field read on data already in hand, not a new param scan.
- Implementation gate (corrected): `WorldFarmableEnemy ⇔ disableRespawn == 0 AND already classified as
  non-boss/non-named-NPC`. Reuse the existing `WorldBosses` / named-NPC classification to exclude those
  first, THEN treat `dr==0` on remaining trash as farmable. Do NOT rely on `dr==1` to auto-exclude
  bosses — bosses read 0. Read `disableRespawn` from the `NPCParamID` of the *placed* enemy (the same
  name can map to a friendly row `dr=0` and a boss row `dr=1`, e.g. Castellan Edgar `523110000` vs
  `533110000`).

**(b) `WorldFarmableCollectible`** — gatherable/killable-drop object that respawns on grace rest.
- **VERIFIED 2026-07-01** (`tools/verify_farmable_collectible.py`, vanilla+ERR — see findings doc). The
  original field guess (`getItemFlagId01/02/03`, "any of 3 slots == 0") is WRONG: there are 8 per-slot
  fields `getItemFlagId01..08` and **all of them are always 0** across the entire table (unused override
  slots, "0 = use shared"). The authoritative field is the **single master `getItemFlagId`** (paramdef
  "0 = flag disabled").
- Polarity CONFIRMED: `getItemFlagId == 0` → farmable · `!= 0` → tracked (one-time uniques: Larval Tear,
  Dragon Communion Seal, …). BUT nonzero is not automatically one-time — some nonzero flags are
  *repeatable* (no save bit). Correct test: `getItemFlagId == 0 OR flag_is_repeatable(getItemFlagId)`.
- Already read live and already resolved: `resolve_loot_flag` reads `getItemFlagId @ +0x80`
  (`src/goblin_inject.cpp:4720`) and `flag_is_repeatable` exists (`:4679`). So this needs **zero new
  plumbing** — just a category branch on the value `resolve_loot_flag`/`flag_is_repeatable` already
  compute at the loot site.
- Composition with (a): an enemy-drop collectible is only farmable if the enemy respawns too
  (`enemy.disableRespawn == 0`); map gathering nodes gate on the lot flag alone.

Both: new `Category::` enum entries + `category_meta.cpp` icon/scale rows (see mechanism note below).

## Part B — the 31 MapGenie gaps, grouped by likely data source

Mechanism for adding any category (confirmed via 2 existing examples — Site of Grace `iconId` read at
`goblin_inject.cpp:972-1007`, and the item taxonomy classifier `goblin_inject.cpp:~1149-1215` keyed on
`EquipParamGoods.goodsType + sortGroupId`): **(1)** add `Category::X` to the enum, **(2)** add an icon-key
+ `{Category, MENU_MAP icon string, scale}` row in `src/worldmap/category_meta.cpp` (two parallel tables,
~line 65-92 and ~line 177+), **(3)** add the classification branch at the relevant live-read site in
`goblin_inject.cpp` (or `map_entry_layer.cpp` / `grace_layer.cpp` / `map_renderer.cpp` depending on source).

**Tier 1 — LOW effort, existing classifier table, just needs new cells (VERIFIED mechanism, not the data).**
Item-family gaps — these almost certainly slot into the *existing* `(goodsType, sortGroupId)` →
`Category` table at `goblin_inject.cpp:~1149-1215` (Remembrance/Talisman Pouch/Tool/Scarabs/Key Item are
all `EquipParamGoods` subtypes, distinguished by `sortGroupId` or item-ID range, same shape as every
already-wired Loot/Key category):
- Key Item (generic) — 85 · Remembrance — 25 · Talisman Pouch — 3 · Tool — 30 · Cerulean Scarab — 21 ·
  Crimson Scarab — 40 · Item (generic) — 13 · Miquella's Cross — 13 (DLC-specific, likely same table)

**Tier 2 — `WorldMapPointParam.iconId`-keyed. VERIFIED 2026-07-01** (`tools/verify_worldmap_iconids.py`,
vanilla+ERR — see findings doc). The numbers below were MapGenie pin-COUNTS, not iconIds. Real result:
iconId is a stable, mod-agnostic key (identical vanilla↔ERR), but **only ~half these categories are
actually in `WorldMapPointParam`**:
- **Wireable via iconId (subset A):** Divine Tower = **23** · Evergaol = **9** · Minor Erdtree = **30** ·
  Grand Lift = **21** (only the 2 grand lifts, NOT the 40 in-dungeon elevators) · "Dungeon" = the UNION
  of typed minor-dungeon icons {Catacombs 4, Caves 13, Tunnels 14, Hero's Graves 16, Wells 15, DLC 230/
  231/234} · "Legacy Dungeon" = per-location UNIQUE icons {Stormveil 50, Raya Lucaria 51, Haligtree 55,
  Elphael 56, Volcano Manor 58, Farum Azula 59, Leyndell 60, Shunning-Grounds 61, Carian Study Hall 66,
  DLC 210/211/213/218}. Classify by single value (first four) or set-membership (Dungeon, Legacy).
- **NOT in WorldMapPointParam (subset B) — need a different source, do NOT scope as WMPP work:** Smithing
  Table, Stone Cairn, Hidden Passage (AEG/MSB interactables) · Martyr Effigy (summoning pools — reuse
  `tools/generate_summoning_pools.py`) · Portal/waygate (MSB warp assets; iconId 87 is impure) ·
  Wandering Mausoleum (dynamic entity; iconId 45 = only the static Mohgwyn one) · Dragon Shrine (folds
  into Churches iconId 3, no distinct icon — ambiguous) · Landmark (MapGenie catch-all — skip).
- Original count list (for reference): Divine Tower — 6 · Dragon Shrine — 2 · Dungeon — 64 ·
  Elevator — 40 · Evergaol — 11 · Hidden Passage — 59 · Landmark — 172 · Legacy Dungeon — 7 ·
  Martyr Effigy — 212 · Minor Erdtree — 11 · Portal — 39 · Smithing Table — 1 · Wandering Mausoleum — 7
  · Stone Cairn — 5

**Tier 3 — `NpcParam` teamType/npcType. VERIFIED 2026-07-01** (`tools/verify_npc_teamtype.py` — see
findings doc). The "one shared teamType/npcType pass emits all six" hypothesis is WRONG: `teamType` is a
per-ROW combat allegiance (each NPC spans 3–6 teams across its state rows), and `npcType` is effectively
all-0 (the `==1` set is 4 named rows). Only ONE of the six is a clean param signal, and it is already
shipped:
- **Ghost = NPC invader = `teamType ∈ {24,27}` ∧ `nameId>0` = the existing `WorldHostileNPC`** (live @
  +0x133, `goblin_inject.hpp:360`, `map_entry_layer.cpp:1256`). No new work.
- **Merchant** → `ShopLineupParam` presence (different param), not NpcParam.
- **Character** → the already-merged `QuestNpcLayer` set (`docs/memory/features/quest-browser.md`).
- **Trainer** (count 1) → single hand-identified NPC.
- **Elite Enemy / Enemy** → notability datamine (`tools/datamine_enemy_notability.py`), not a teamType.
- Original count list (reference): Character — 127 · Ghost — 57 · Merchant — 43 · Trainer — 1 ·
  Elite Enemy — 184 · Enemy — 82

**Tier 4 — INVESTIGATED 2026-07-01 (see findings doc). Verdict: not RE-able / already covered.**
- **Lore — 6, Miscellaneous — 9: NO game-data source — do not scope as RE work.** Both are MapGenie
  "Other (guide annotations)" = human editorial pins, not derived from any param/MSB/EMEVD field. The
  only source (MapGenie's content) is explicitly rejected (ToS/scraping + MapGenie-space coords, see
  `quest-browser.md`). Recommend dropping from the coverage target as permanently-uncovered wiki notes.
- **Quest (steps) — 7: ALREADY COVERED, no new code.** The mod already draws Quest-Progression (73) +
  Seedbed-Curses (7) + the runtime `QuestNpcLayer` (71 NPCs, live-verified). MapGenie's 7 are a subset.

## Open questions before implementation starts

1. ~~Tier 2/3 are mechanism *hypotheses*, not verified against actual rows — budget an RE pass.~~
   **RESOLVED 2026-07-01:** RE pass done (`tools/verify_*.py`, findings doc). Tier 2 = iconId key for
   the named-location subset (Divine Tower 23 / Evergaol 9 / Minor Erdtree 30 / Grand Lift 21 / Dungeon &
   Legacy sets); the rest aren't WMPP. Tier 3 collapses to "Ghost = existing WorldHostileNPC"; the other
   5 need ShopLineupParam / QuestNpcLayer / notability-datamine, not a teamType pass.
2. ~~Confirm `disableRespawn` varies as expected on trash vs boss rows before wiring Part A(a).~~
   **RESOLVED 2026-07-01:** it does NOT vary as hoped — bosses read `dr=0`, not `1` (gated by event
   flags, not this field). Gate corrected above to `dr==0 AND non-boss`. See findings doc.
3. ~~For Part A(b), confirm which of `getItemFlagId01/02/03` to check.~~ **RESOLVED 2026-07-01:** none of
   them — the per-slot 01..08 fields are always 0. Authoritative field is the single master
   `getItemFlagId` (already read live @ +0x80). Farmable ⇔ `==0 OR flag_is_repeatable`. See findings doc.
