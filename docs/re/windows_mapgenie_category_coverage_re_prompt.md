# RE/verification brief — the 31 MapGenie category gaps + 2 custom respawn categories

## Scope of THIS prompt: verify hypotheses only, do not implement yet

`docs/plans/mapgenie_category_coverage_plan.md` (read it first, it's short) already scopes the
mechanism for all 33 items. Its Tier 2/3 category→data-source mappings and both Part A fields are
**pattern-matched hypotheses**, not verified against real live game data. Its own sequencing note
says the actual `Category::` enum edit should wait for `generated_data_removal_plan.md` Phase B
(dedup the enum into `generated_shared/` so it's only edited in one file instead of 4 duplicated
`generated_*/goblin_map_data.hpp` copies) — Phase B is blocked on Phase A (regenerate+rebuild the
vanilla/erte/convergence profiles), which hasn't happened yet either.

**So: this session should NOT edit `Category` enums or wire new categories into
`goblin_inject.cpp`/`category_meta.cpp` yet.** Its job is to turn every "hypothesis" in the plan
into a **verified, cited fact** (real field values read from the live game / param dumps / MSB),
so that once Phase B lands, implementation is copy-paste-confident instead of another RE session.
If you find verification is fast and trivial for a whole tier, it's fine to ALSO stage the actual
code change on a branch for later — just don't merge/land it ahead of Phase B without checking
back in first.

## What already exists (reuse, don't re-derive)

- Live param read path: `from::params::get_param` (used for `NpcParam`, `ItemLotParam_map`,
  `ItemLotParam_enemy`, `WorldMapPointParam`, `EquipParamGoods` already).
- Existing classifier examples to pattern-match against:
  - Site of Grace `iconId` read — `goblin_inject.cpp:972-1007`
  - Item taxonomy classifier (`goodsType` + `sortGroupId`) — `goblin_inject.cpp:~1149-1215`
  - Live loot / `ItemLotParam` read — `goblin_inject.cpp:1072-1101`
- `tools/paramdefs/*.xml` — field defs + Japanese labels for every param this plan touches.
- `tools/_find_npc.py` — MSB placement lookup, if any Tier 3 category needs a real NPC/placement
  cross-check rather than just a param field read.

## Verification tasks, in the plan's own priority order

### Part A — the 2 custom respawn categories (concrete fields, just need live confirmation)

1. **`WorldFarmableEnemy`** — `NpcParam.disableRespawn` (u8, JP "リスポン禁止か" = "respawn
   prohibited?"). Read this field on:
   - A known respawning trash mob (e.g. a common Soldier/Wolf row) → expect `0`.
   - A known one-time boss row (e.g. a Rune Bear or field boss) → expect `1`.
   - **Confirm it's actually `0` for farmable trash and `1` for bosses on REAL rows** — the plan
     flags this as unverified. If any boss row unexpectedly reads `0`, note it (means
     `disableRespawn` alone isn't a safe gate and needs a 2nd condition, e.g. combined with the
     existing boss/NPC classification already in `goblin_inject.cpp`).

2. **`WorldFarmableCollectible`** — `ItemLotParam*.getItemFlagId01/02/03` (u32, JP "取得済みフラグと
   ザクザク枠兼用(0:共通使用)"). A lot can roll up to 3 items. Determine, on real rows:
   - Is checking "ANY of the 3 slots == 0" correct, or is there a canonical single slot (e.g. only
     slot 1 ever matters, 2/3 are always mirrors or always 0 regardless)?
   - Cross-check against a KNOWN farmable material (e.g. a common crafting mat that respawns) vs a
     KNOWN one-time pickup (e.g. a key item / unique drop) to confirm the flag's polarity in
     practice, not just from the JP label.

### Tier 2 — `WorldMapPointParam.iconId`-keyed hypothesis (same pattern as the already-wired Site of
Grace `row.iconId` read)

For each category below, find the real `iconId` value(s) it corresponds to (there may be more than
one iconId per category — note all of them):
Divine Tower, Dragon Shrine, Dungeon, Elevator, Evergaol, Hidden Passage, Landmark, Legacy Dungeon,
Martyr Effigy, Minor Erdtree, Portal, Smithing Table, Wandering Mausoleum, Stone Cairn.

Method: read `WorldMapPointParam` live (or dump it), correlate `iconId` against the MENU_MAP icon
name it resolves to (existing `category_meta.cpp` icon-string tables show the naming convention,
e.g. `MENU_MAP_01_Bonfire`), and against a known in-game instance of that landmark type (e.g. a
known Evergaol location) to confirm the row you're reading really is that category, not a
neighboring one. Where multiple MapGenie categories could plausibly share one iconId, flag the
ambiguity explicitly rather than guessing.

### Tier 3 — `NpcParam`-based hypothesis (teamType / npcType)

Character, Ghost, Merchant, Trainer, Elite Enemy, Enemy — likely share ONE classification pass.
Find the real `teamType`/`npcType` (or whichever field actually discriminates) values for each,
verified against known real NPCs of each kind (e.g. a known merchant NPC id vs a known "Ghost"
lore-item NPC vs a generic hostile). Note whether this can share code with the Part A(a)
`disableRespawn` read (same `NpcParam` row, so likely one read site can emit multiple fields).

### Tier 4 — do NOT attempt blind

- Lore (6), Miscellaneous (9): no hypothesis exists yet. If time allows after Tiers 1-3, spend a
  SHORT investigation pass (what MapGenie actually puts in these 2 categories — check
  `docs/coverage_vs_mapgenie.md` for what's currently uncovered) and report a hypothesis, don't
  force an implementation.
- Quest (7): **skip entirely.** Per the plan, this is very likely already covered by
  `feat/quest-npc-layer`'s `QuestNpcLayer` (already merged to master — see
  `docs/memory/features/quest-browser.md`). Just confirm this belief holds (spot-check whether
  MapGenie's 7 "Quest" pins line up with QuestNpcLayer's current pins) — do not write new code for
  it either way in this session.

## Deliverable

`docs/re/windows_mapgenie_category_coverage_re_findings.md`:
- Per tier/category: the verified field + value(s), the real row(s) used to confirm it, and either
  "CONFIRMED matches hypothesis" or "hypothesis WRONG, real mechanism is X" with citation.
- Explicitly call out anything still unresolved after this pass (better an honest "couldn't verify
  X, here's what I tried" than a guessed value baked in as fact).
- Update `docs/plans/mapgenie_category_coverage_plan.md` itself: replace "hypothesis" language with
  confirmed values inline once verified, so the plan is implementation-ready the moment Phase B
  lands.
