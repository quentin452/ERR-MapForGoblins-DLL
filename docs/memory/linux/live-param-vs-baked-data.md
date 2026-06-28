---
name: live-param-vs-baked-data
description: "Direction — replace baked generated/* snapshots with LIVE param/FMG reads to kill per-mod drift; hybrid (params live, MSB/EMEVD baked)"
metadata: 
  node_type: memory
  type: project
---

**User direction (asked twice, 2026-06-19 & 06-20): can we drop the baked `src/generated*` (generated_vanilla / generated / convergence) and read directly what the game/mods expose in memory, since the baked snapshots drift between mods?**

**YES for PARAM-backed data — and the project already has the keys.** ER keeps everything live in `SoloParamRepository` (params) + `MsgRepository` (FMG text). The mod already uses them: `from::params::get_param<T>(L"ParamName")` (generic live param-by-name; used for ItemLotParam_map), `find_param_res_cap_by_name` / `find_world_map_point_param_res_cap` (walk the ParamResCap list; the new `world_map_param_ready()` poll uses this), `MsgRepositoryImp` (live FMG — item names, PlaceName inject), live-loot reads ItemLotParam.getItemFlagId live. So reading these LIVE instead of baking = zero per-mod re-bake, zero drift, randomizer-safe, mod-agnostic (ERR/Convergence/ERTE/vanilla all just work):
- WorldMapPointParam (marker rows: area/grid/pos/iconId/eventFlag)
- WorldMapLegacyConvParam (the dungeon→overworld conv — the [[overlay-rendered-markers]] step-1 table)
- WorldMapPlaceNameParam (region origins)
- ItemLotParam (loot flags — already live), Goods/Weapon/etc names via FMG (already live)

**Limit → HYBRID is the realistic target.** NOT everything is a param, so these stay BAKED offline (tools/extract_*.py): AEG099 gathering-node MSB geometry (not a param), EMEVD event-flag↔item correlations (event files), MFG's iconId→category curation/mapping (mod's own logic). So: live params for what's in regulation, baked for MSB/EMEVD/curation.

**Caveat:** live param reading gives the SAME area-local coords + same per-page render transform need ([[overlay-rendered-markers]] marker→render-space) — it fixes COHERENCE/drift, not the math. Headless/cron contexts: param access needs the game running.

**Q (2026-06-20): can Legacy conv be REMOVED entirely via RE? NO — only its baked snapshot.** Two distinct chained transforms: conv (PIECEWISE — one translation per dungeon base-point, data-driven) THEN the single shared affine render=M·world+T (RE target #1, [[overlay-rendered-markers]]). Conv is piecewise so the affine CANNOT absorb it — RE gives the affine, conv stays a separate lookup. And because overlay-markers inject ZERO rows (to kill freeze), the game never computes our markers' positions → we MUST do conv ourselves (letting the game do it = re-inject = freeze returns). What live-read removes = the BAKED LEGACY_CONV table (→ exact, drift-free, mod-agnostic), NOT the conv STEP (a tiny permanent lookup). Bonus: even the #1 hook-RE pairs need conv first — legacy graces sit in sub-area coords at point+0x80, must be conv'd to overworld before fitting M or M won't come out shared per page.

**ARCHITECTURE Q&A — what's live-readable vs what still needs the transform (consolidated 2026-06-20, so we don't reloop):**
- **Undiscovered graces don't need the 3D world — they're ALREADY in WorldMapPointParam.** The param is static: EVERY grace row exists regardless of discovery; resting/discovery only flips a visibility event-flag (textDisableFlagId / display-group, read live via VerifyEnable/DisableEventFlag, re_v59 FUN_140d58470). So "find the undiscovered graces" = read the param, not the 3D world.
- **3D world pos ≠ map pos.** The world map is a stylized Scaleform asset, NOT a top-down projection of 3D XYZ. Reading a grace's 3D position would ADD a 3D→map transform (another unknown, not a clean affine) ON TOP of M/T — more work, not less. get_player_map_pos already does 3D→map for the player but only re-derives what WorldMapPointParam gives directly.
- **WorldMapPointParam ≠ calibration — two ORTHOGONAL axes.** (1) DATA SOURCE: bake → live WorldMapPointParam (kills drift, mod-agnostic, includes undiscovered). (2) TRANSFORM: conv + M·world+T + screen (the calibration, [[overlay-rendered-markers]]). The param holds map-space (area/grid/pos) = the SAME numbers we bake, NO render coords → live-reading changes the SOURCE, not the transform. M/T stays mandatory (re_v59 confirmed it can't be skipped). The param IS the world_in half of each calib pair (point+0x80) but can't give M/T without render_out. ⇒ the bake CAN be dropped (live param), M/T CANNOT; independent, any order.
- **3D entity detection (WorldChrMan) is NOT the quest cheat code.** It sees only LOADED NPCs near the player (ER streams the world → far NPCs absent from memory) and gives presence/pos/id/alive, NOT quest STATE/STEPS (those live in EMEVD/ESD, not the entity). Quest progress = EVENT FLAGS, already live-read ([[thread7-coverage-detector]] SetEventFlag hooks, quest gates, EventFlag_C1). Full-map quest NPCs still need static MSB placement + step text (hand-authored/wiki, [[quest-browser]]). 3D's only niche = a local "nearby NPC/merchant" radar.

**Status:** noted as a direction; not started. Incremental (infra exists). Pairs with overlay-markers v2 (live-param-driven). The diff tool [[mapforgoblins-map-freeze]]/diff_map_data.py detects drift in the meantime.

## DONE: graces are now LIVE in the overlay (2026-06-20, commit e7618ec)
The ImGui overlay path no longer uses baked grace data. `goblin::capture_live_graces()`
reads WorldMapPointParam live (rows with **iconId 370** = Site of Grace), captured at init
BEFORE inject_map_entries() swaps the param backing (else it reads our injected rows).
`goblin::live_graces()` returns the list; the overlay's grace loops iterate it. Log line
`[LIVE-GRACE] captured N grace rows`. Injection path untouched (still baked). This proves the
live-param direction for one category; other categories still need baked curation
(iconId→category, AEG099 geometry). All 3 grace map pages already place correctly (see
[[overlay-rendered-markers]]).

## ❌ CORRECTION: graces are NOT live-readable from WorldMapPointParam (2026-06-20, commit 03c68c7)
The note above ("graces are now LIVE") is WRONG. The live WorldMapPointParam does NOT contain
the grace map-pins. In the base game grace pins are generated from **BonfireWarpParam** at
runtime (not stored in WMP); the project AUTHORS the 427 grace rows from
`data/massedit/World - Graces.MASSEDIT` (positions extracted OFFLINE from MSB/BonfireWarpParam)
and injects them. Evidence: live read iconId 370 = 0 rows; ICON_FRAME_OFFSET=0 for ERR; a
sample baked grace row (110000) resolves live to iconId 60 area 11; live iconId histogram tops
~261, no value near 427. So `capture_live_graces()` tries live then falls back to the BAKED
grace set (= the offline-extracted game data). The live_graces() seam + overlay refactor stay.
A TRUE live grace path = read BonfireWarpParam + resolve each bonfire's MSB position (MSB
geometry isn't a param → still needs offline extraction or runtime MSB walking). Lesson: "ER is
data-driven" holds, but grace POSITIONS live in MSB geometry, not one queryable param.

## ❌ World-* categories are NOT live in WorldMapPointParam either (2026-06-23, commit c4f2659 revert)
Tried the "cheap baked→runtime" win: read the World-* categories (Stakes/SummoningPools/SpiritSprings/
Maps/Paintings/ImpStatues/Hostile+QuestNPC/Interactables) LIVE from WorldMapPointParam by ERR row-id
range (the bake assigns each "World - X" MASSEDIT a distinct ID block 75xxxxx..94xxxxx). **In-game probe
([WORLDLIVE]) REFUTED it:** the deployed ERR regulation's live WorldMapPointParam holds only **740 rows
total = 217 textId2==5100 bosses + ~523 STRUCTURAL nav points (iconId=83, textId1/2=-1, ids 78500 +
95100-95413)**. The named World rows the bake expects are **absent live** → 0/523 matched any range. So
the `data/massedit` World files are a different ERR build / never applied to the shipped param. **Lesson
(now 3×): only bosses (textId2==5100) + graces (BonfireWarpParam) are genuinely live-portable; World cats
+ loot + NPCs stay BAKED.** The baked MAP_ENTRIES (8653) ≫ live WMP (740) because most of our data is
MSB/curation, not param. Reverted the feature; negative result in build_buckets comment. Don't retry.

## ⚠️ BAKED PARAMDEF REF IS STALE — real layout drift (2026-06-22, measured)
`tools/paramdefs/*.xml` (used by the OFFLINE pipeline `extract_all_items.py` via SoulsFormats
ApplyParamdef → reads fields BY NAME → computes offsets from the def) is a PRE-SOTE paramdef set that
**drifts from the live 2.6.2.0 struct layout**. Measured (computed total field size vs the LIVE row
stride, ground truth): **Weapon repo=673 vs live 664 (+9), Goods 178 vs 176 (+2), Gem 102 vs 96
(+6); Protector & Accessory exact.** Cross-checked against the CURRENT SOTE Paramdex
(**Nordgaren/Erd-Tools** `Documentation/Params/Defs-English/*.xml`, commit 2026-03) whose totals
**== live for all 5**. So Erd-Tools = current, our repo paramdef = stale.
- **iconId is SAFE** — every divergence is AFTER the iconId field (Weapon div@~0x17c vs iconId 0xBE;
  Goods div@0x40 vs 0x30; Gem div@0x36 vs 0x04) → the solved iconId offsets (Weapon 0xBE / Protector
  0xA6,0xA8 / Accessory 0x26 / Goods 0x30 / Gem 0x04, see [[overlay-icon-atlas]]) stand.
- **Pipeline impact = SMALL but REAL:** of the fields `extract_all_items.py` reads, only **Weapon
  `wepType` is mis-read** (repo computes 0x1A7, live 0x1A6 → off by 1 → wrong weapon type in
  weapon_db). `sortId` (all params, early) and Goods `goodsType` (0x3E, before the 0x40 div) are
  BEFORE the divergence → correct. So shipped data is mostly fine; weapon wepType is the one latent
  bug.
- **FIX (one-shot, optional):** replace `tools/paramdefs/*` with Erd-Tools' Defs-English (verified
  total==live) → fixes wepType + future-proofs any late-field read. Validated via
  `tools/paramdef_iconid_offset.py <field> <xml>` (computes any field offset; confirmed iconId/total
  vs live). Same lesson as the rest of this file: the baked ref drifts; the live build / current
  Paramdex is truth.
