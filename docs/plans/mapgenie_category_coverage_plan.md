# Plan — close the 31 MapGenie category gaps + 2 custom respawn categories

Status: scoped, not started. Research done via subagent (2026-07-01), findings below are grounded in
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
  "respawn prohibited?"). `0` = respawns (farmable) · `1` = unique/one-time (bosses, named NPCs).
- `NpcParam` is already read live in `src/goblin_inject.cpp` (existing enemy classification path) — this
  is one additive field read on data already in hand, not a new param scan.
- Implementation: at whichever `goblin_inject.cpp` site currently assigns `Category::WorldHostileNPC` /
  `WorldBosses`, branch on `disableRespawn == 0` → `WorldFarmableEnemy` instead (probably want this
  to apply only to non-boss/non-named trash mobs — bosses already have their own category and
  `disableRespawn` should always be 1 for those anyway, but verify against a real boss row before relying
  on that as the sole gate).

**(b) `WorldFarmableCollectible`** — gatherable/killable-drop object that respawns on grace rest.
- Field: `tools/paramdefs/ItemLotParam*.xml` → `getItemFlagId01/02/03` (u32). Japanese description:
  "取得済みフラグとザクザク枠兼用(0:共通使用)" = "shared use of acquired-flag and farm-slot (0 = common/shared
  use)". `0` = no persistent one-time flag → infinitely farmable · non-zero = persistent flag → one-time.
- `ItemLotParam` (map + enemy variants) already read live: `src/goblin_inject.cpp:1072-1101`
  (`ItemLotParam_map`/`ItemLotParam_enemy` via `from::params::get_param`). Same shape — additive field
  read, not new plumbing.
- Implementation: wherever a lot-backed marker's category is currently assigned (the live-loot path,
  `goblin_inject.cpp` near the ItemLotParam read), branch on `getItemFlagId0{1,2,3} == 0` (need to confirm
  exactly which of the 3 slots is authoritative for a given lot — likely "any of the 3 is 0" since a lot
  can have up to 3 rolled items) → `WorldFarmableCollectible`.

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

**Tier 2 — LOW-MED effort, hypothesis: `WorldMapPointParam.iconId`-keyed (same pattern as the already-wired
Site of Grace `row.iconId` read).** Needs per-category iconId value lookup, not new plumbing:
- Divine Tower — 6 · Dragon Shrine — 2 · Dungeon — 64 · Elevator — 40 · Evergaol — 11 · Hidden Passage — 59
  · Landmark — 172 · Legacy Dungeon — 7 · Martyr Effigy — 212 · Minor Erdtree — 11 · Portal — 39 ·
  Smithing Table — 1 · Wandering Mausoleum — 7 · Stone Cairn — 5

**Tier 3 — MED effort, hypothesis: `NpcParam`-based (teamType / npcType), same family as Part A's
`disableRespawn` read — likely the same classification pass can emit several of these together:**
- Character — 127 · Ghost — 57 · Merchant — 43 · Trainer — 1 · Elite Enemy — 184 · Enemy — 82

**Tier 4 — unresolved / cross-plan, do not start blind:**
- Lore — 6, Miscellaneous — 9: no hypothesis yet, needs its own short investigation pass before scoping.
- Quest (steps) — 7: **don't implement here** — this is very likely what
  `docs/plans/feat_quests_implementation_plan.md`'s `QuestNpcLayer` already produces once that plan lands
  (per-step markers for the active quest step). Check after that plan ships whether this MapGenie category
  is already satisfied before writing new code for it.

## Open questions before implementation starts

1. Tier 2/3 are mechanism *hypotheses* from pattern-matching existing code, not verified against the
   actual param/MSB rows for each of the 20 categories in them — budget an RE/verification pass per
   category (or per cluster, if several share one source) before estimating real effort.
2. Confirm `disableRespawn` actually varies as expected on a real trash-mob row vs a boss row before
   wiring Part A(a) — the hypothesis is strong (field name + JP label match exactly) but unverified live.
3. For Part A(b), confirm which of `getItemFlagId01/02/03` to check (one canonical slot vs "any of three").
