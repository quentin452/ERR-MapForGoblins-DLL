# Plan — replace ALL baked data with the runtime/disk path

Status: scoped, not started. Supersedes-and-subsumes `generated_data_removal_plan.md` (that plan is the
marker-position subset = Phase 0 here).

## Goal (prime directive)

Per `AGENTS.md`: **Runtime/Disk over baked.** Read icons, glyphs, markers, names, and param data from the
ACTIVE install's real files (resident GPU textures, or disk via the Oodle/dvdbnd no-bake path, or live
`from::params::get_param` / native `GetMessage`) so they are automatically correct for whatever mod is
loaded. Every baked snapshot is an ERR-frozen artifact — stale or wrong under any other mod — and is a
transitional fallback to be **eliminated**, not a source of truth.

**End state:** the DLL produces a correct overlay on any Elden Ring install (vanilla, ERR, Convergence,
ERTE, arbitrary mod) with NO per-profile baked data compiled in, except a small irreducible core of
genuinely-authored content (curated dicts with no in-game source) which is mod-agnostic *by being
hand-written to be so*.

**Acceptance test for every item below:** "does this still produce a correct result on a DIFFERENT mod
with different params/textures/MSB?" If it only works because the baked values happen to match ERR, it is
not done.

## Classification — every baked/generated artifact falls in one of three buckets

- **(M) Migratable to runtime** — an in-game source exists on every install; the bake is pure duplication
  that goes stale under other mods. **Must be moved to the runtime/disk path.** This is the bulk of the work.
- **(A) Authored, stays** — no param/MSB/EMEVD/FMG source exists; it is a curated dict MapForGoblins wrote
  (mod-agnostic because it's hand-written that way, e.g. AEG-model→category tables, category exceptions).
  Stays compiled, but should be **profile-independent** (one copy in `generated_shared/`, never a per-mod bake).
- **(H) Housekeeping** — real + byte-identical across all profiles; not a mod bake, just duplicated 4×.
  Dedup into `generated_shared/`, no runtime work.

The existing runtime infrastructure the migrations lean on (all already shipped, verified this cycle):
- **Live params:** `from::params::get_param` (NpcParam, ItemLotParam_map/_enemy, WorldMapPointParam,
  EquipParamGoods, BonfireWarpParam) — resident, mod-correct. Verified whole-table read this session
  (`tools/verify_*.py`).
- **Native name resolution:** `CS::MsgRepositoryImp::GetMessage` (RE done, `docs/re/windows_native_msg_getter_re_findings.md`)
  — resolves any FMG string (NpcName, GoodsName, PlaceName, …) on the active install, layered DLC-correct.
- **Disk MSB + EMEVD:** `msbe::parse_emevd` / `parse_emevd_quest_npcs` + `read_game_file_decompressed`
  (dvdbnd + Oodle no-bake) — runtime-verified mod-agnostic (71 quest NPCs on ERR, `quest-browser.md`).
- **Disk glyphs / resident textures:** the grace-icon live-harvest (`ensure_grace_srv`) + `map_point_glyph_uv`
  disk path already prove native-from-disk icon resolution works.

## Inventory — the full baked surface, per artifact

| Artifact | Bucket | In-game runtime source | Status / notes |
|---|---|---|---|
| **Marker positions** `goblin_map_data` `MAP_ENTRIES` | M | disk MSB + loot `ItemLotParam` + EMEVD (already the live path) | **~done** — already an empty stub for ERR/vanilla; formal deletion = `generated_data_removal_plan.md` Phase C. This is Phase 0 here. |
| **World-feature markers** (Stakes, Imp Statues, Hero's Tomb, Paintings, Maps, Spirit Springs, Kindling, Summoning Pools, Seal Puzzles, Gestures, Hostile NPC, Material Nodes) | M | disk MSB/AEG placement + `world_feature_assets` model→category table (authored) | Partly migrated (disk hostile-NPC pass, `goblin_world_feature_models`, kindling). Each still-MASSEDIT-baked feature must move to a disk pass keyed on the active MSB/AEG. The model→category table itself is (A). |
| **Quest NPCs** `goblin_quest_steps` entities/flags | M | runtime EMEVD extractor `parse_emevd_quest_npcs` (live-verified, 71 NPCs) | Entities/concluded-flags/registers already runtime + mod-agnostic. Only the **step title/desc prose** is (A) — hand-authored, stays. |
| **Enemy names** `goblin_enemy_names` | M (partial) | native `GetMessage` → `NpcName` FMG of the active install | The removal plan called this "not removable" — but that predates the GetMessage RE. NpcName exists on EVERY install; resolve live. ERR-Codex-only names that aren't in NpcName become a thin (A) override, not the whole 370 KB table. |
| **Item DB** `items_database.json`, `item_icon_table.json` | M | live `EquipParamGoods`/`ItemLotParam` + `GetMessage` (GoodsName/WeaponName/…) + live icon id | Item identity/name/icon already resolved live at the loot site (`resolve_loot_item_textid`, `encode_live_item`). Retire the JSON bake as a source of truth; keep only as offline analysis output. |
| **Icon atlas** (baked overlay icon atlas) | M | resident GPU MENU textures / disk via Oodle+dvdbnd | The prime-directive example. Replace `native → baked → circle` with **`native-from-disk → circle`**. Baked atlas is the ERR-frozen middle tier to delete. |
| **Grace positions** `grace_position_index` | M | live `BonfireWarpParam` (GraceLayer already reads live) | Bake already a fallback; drop it. |
| **Region anchors / name regions** `goblin_region_anchors`, `goblin_name_regions` | M?/A | `WorldMapPointParam` + `WorldMapPlaceName` (place names verified this session) | Assess: much is derivable from WorldMapPointParam (see Tier-2 findings) + PlaceName via GetMessage. What isn't → (A). |
| **World-map layout** `goblin_tile_tabs`, `goblin_major_regions` | H | base-game world-map tab layout | Real + identical on every profile incl. ERR; not mod-specific. Dedup into `generated_shared/`. |
| **Model aliases** `goblin_model_aliases` | A/H | (none — curated model-id→label dict) | Authored; ERR-only feature per `gen_nonerr_stubs.py`. Keep for ERR, stub elsewhere (already the pattern). |
| **Category exceptions / name aliases** `goblin_category_exceptions`, `goblin_name_aliases_en` | A | (none — curated dicts, no param/MSB source) | The irreducible authored core. Stays compiled; dedup 4×→1 in `generated_shared/`. |

## Phasing

**Phase 0 — marker positions (already in flight).** Execute `generated_data_removal_plan.md` A→B→C. This
proves the disk marker path is the sole source on all 4 profiles and removes the `MAP_ENTRIES` machinery.
Everything below assumes Phase 0's `generated_shared/` dedup exists.

**Phase 1 — names go live (highest ROI, infra already exists).** Route enemy + item + place names through
native `GetMessage` instead of the baked `goblin_enemy_names` / `items_database` name fields. Reduces the
biggest baked table (370 KB) to zero. Acceptance: DLC + non-ERR item/enemy names resolve correctly with no
`#ifdef MFG_*` and no baked name table.

**OFFLINE-VERIFIED 2026-07-01 (`tools/verify_enemy_name_runtime.py`, vanilla) — redirects the approach:**
- The enemy bake exists because markers encode the name at `enemyId + 900000000` where `enemyId` follows
  ERR's `model*1000+variant*100+4` TutorialTitle convention. Vanilla's own TutorialTitle has **0 of the
  520** ids → the `+900M → TutorialTitle` lookup finds nothing on non-ERR. So the bake can NOT be dropped
  in favour of the install's TutorialTitle.
- BUT all 520 names ARE in vanilla's **`NpcName`** FMG. The correct mod-agnostic path is NOT the
  model-convention id but the enemy's actual **`npcParamId`** (already in hand at the disk enemy/loot
  pass) → `NpcParam.nameId` → `GetMessage(NpcName, nameId)`. This is verbatim the shipped hostile-NPC
  path (`npc_team_and_name` → `lookup_text_utf8(nameId + 700000000)`, Tier-3 findings). (A naive
  `model*10000+variant*1000` bridge resolved 426/520 but mangled variants — proof the *data* is present;
  use the real npcParamId, not a formula.)
- **⇒ Phase 1 for enemy names = label the enemy-drop marker from its source `npcParamId` at the disk
  pass (reuse the hostile-NPC mechanism), then delete `goblin_enemy_names` + the +900M PlaceName
  injection (`goblin_messages.cpp:473`). No new table, no TutorialTitle dependency.**

**Phase 2 — world-feature markers fully disk-sourced.** For each still-baked world-feature category, add
(or extend) a disk pass reading the active MSB/AEG/EMEVD, gated by its category toggle, dropping the
MASSEDIT→map_data bake. The `world_feature_assets` model→category table stays as (A) in `generated_shared/`.
Acceptance: each feature draws correctly on a mod that moved/added instances.

**Phase 3 — icon atlas → native-from-disk.** Replace the baked overlay icon atlas with disk/GPU glyph
resolution (`native-from-disk → circle`), deleting the atlas bake. Acceptance: icons correct under a mod
with different MENU textures; missing glyph falls back to the plain circle, never a wrong baked sprite.

**Phase 4 — geography + residuals.** Migrate region anchors/name regions where param-derivable; dedup the
genuine (A)/(H) remainder into `generated_shared/`. Re-audit that the ONLY compiled data left is the
authored core, one copy, mod-agnostic by construction.

**Phase 5 — retire the bake pipeline.** Once Phases 0–4 land, the MASSEDIT + per-profile codegen stages of
`build_pipeline.py` have no consumers. At THAT point the pipeline (or the relevant stages) can be removed —
`build.bat` and `README.md` updated in the same change. (Deleting it before then breaks the build; see the
`build_pipeline.py` staged deletion — that file must be restored or this phase completed first.)

## Verified state after Phase 1 landed (2026-07-01) — reframes Phase 2 + adds a landmine

Phase 1 (enemy names) landed on master (`5b24392`); deployed + in-game-verified on ERR (log clean,
`[COVERAGE] TOTAL baked=0`, GETMESSAGE PASS, SANITIZE cleared 0). Investigating the next point turned up
facts that change the remaining plan:

- **The world-feature MARKERS are ALREADY runtime-sourced.** `generate_map_data_cpp` writes an
  **unconditional empty stub** (its two downstream consumers were retired), and the deployed ERR log shows
  `baked=0` on 62/63 categories, none `baked>0`. So Phase 2 as originally written ("add disk passes for
  world features") is a **no-op — already done at runtime.** The real Phase-2 work is deleting the
  **~14 dead `generate_*_massedit` stages** (stakes, imp, paintings, maps, gestures, seal, hero-tomb,
  spirit-springs, kindling, summoning, material, hostile-npc, quest-npc, pieces) whose `.MASSEDIT` output
  nothing reads.
- **COUPLING — `generate_loot_massedit.py` is MIXED, do not delete it whole.** Besides dead `.MASSEDIT`
  it also emits `loot_lot_linkage.json` + `item_icon_table.json`, which STILL feed the compiled
  `goblin_category_exceptions` / icon table / `goblin_name_aliases_en`. Drop its MASSEDIT emission, keep
  the JSON. And `item_icon_table.json` is itself the ERR-frozen "placed-item set" → migrating THAT to a
  runtime item enumeration is its own later phase (part of the item-DB row above).
- **LANDMINE created by Phase 1 — non-ERR regen is now a hard prerequisite.** The local non-ERR bakes are
  stale: `generated_vanilla/goblin_map_data.cpp` = **MAP_ENTRY_COUNT 6916**,
  `generated_convergence` = **7448** (ERR/erte = 0). Those stale MAP_ENTRIES reference `+900M` enemy-name
  textIds whose resolver Phase 1 DELETED. So **any non-ERR build now shows empty enemy labels until those
  profiles are regenerated to the empty stub** (= `generated_data_removal_plan.md` Phase A). ERR is fine
  (already empty, verified). This makes Phase A the immediate next step, not optional.

**Reordered next PRs (evidence-based):**
1. **Phase A regen — DONE 2026-07-01h** (merged to master). Reran
   `build_pipeline.py --profile {vanilla,convergence,erte}` → all 4 profiles MAP_ENTRY_COUNT 0; all 3
   non-ERR DLLs rebuilt clean (clang/ninja). Enabler: `build_pipeline.py` now auto-loads
   `tools/sitecustomize.py` (tolerant unlink) on Windows (`fe8d28c`). The old "not possible on the dev
   machine — CMake configure fails" note was the Linux/MSVC framing; THIS Windows box builds all 4 via
   clang-cl+xwin+ninja. Only the **vanilla in-game sanity-check remains (user)**.
2. **Dead-MASSEDIT cull — DONE** (branch `pr2-dead-massedit-cull`, `1be1be4`). Removed the 14 MASSEDIT
   generators + 5 orphaned extractors + `relocating_boss_fix` + the `massedit_dir` param; kept
   `generate_loot_massedit`'s JSON half. Confirmed dead offline: regen ERR before/after (with
   `massedit_generated` wiped) → `src/generated` byte-identical; ERR DLL rebuilds clean. Left for Phase 5:
   the orphaned generator *scripts*, the tracked dead `data/massedit_generated/*.MASSEDIT` artifacts, and
   `generate_loot_massedit`'s own (now-dead) `.MASSEDIT` emission.
3. **item_icon_table → runtime — DONE as a REMOVAL** (branch `pr3-remove-category-exceptions`, `6c19f72`,
   not merged). User chose to DELETE the curated category-exception override rather than re-source it, so
   `item_marker_category` classifies purely by ER's live `(goodsType, sortGroupId)` taxonomy. The generated
   `goblin_category_exceptions.{cpp,hpp}` + DLL lookup + CMake + `generate_data` emitter are gone;
   `item_icon_table.json` has zero compiled consumers (offline-only). Behaviour change: curated splits
   (Golden Runes Low, Smithing Low/Rare, Great Gloveworts, Rune Arcs, …) fold into their parent live
   category. Follow-ups: the now-dead F1 sub-category toggles/enum values + the stale offline taxonomy
   mirror. Awaiting in-game check.
4. **Phase 5** — retire `build_pipeline.py` + `build.bat` + `README.md` once the authored core is all
   that's left.

## Risks / guardrails

- **Do not delete a bake before its runtime replacement is verified IN-GAME on a non-ERR profile.** The
  acceptance test is mod-agnosticism, which only vanilla/Convergence/ERTE can prove — ERR passing is
  necessary, not sufficient.
- **The authored (A) core is real and must survive.** `category_exceptions`, `name_aliases`, the
  `world_feature_assets` model table, quest step prose, and model aliases have NO in-game source. "Replace
  all baked data" means the mod-*derived* bakes, not the hand-authored dicts — those just get deduped, not
  deleted.
- **`build_pipeline.py` is load-bearing until Phase 5.** It generates the `src/generated/*.cpp` compiled
  into the DLL and is called 3× by `build.bat`. It is the LAST thing removed, not the first.
