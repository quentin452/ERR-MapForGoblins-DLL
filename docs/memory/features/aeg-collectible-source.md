---
name: aeg-collectible-source
description: Deterministic AEG asset → item source (AssetEnvironmentGeometryParam.pickUpItemLotParamId) — the no-bake/no-manual-match way to map ERR collectibles; runtime feature loot_collectibles
metadata: 
  node_type: memory
  type: project
---

**The real source for placing/identifying ERR collectibles, zero manual matching** (found
2026-06-24 via param chain + Ghidra-confirmed). ERR's gather/collectible items (Runic Trace, Ember
Trace, fireflies, butterflies, mosses, smithing stones, ...) are placed in the world as **MSB
`Parts.Assets` with a dedicated model `AEG099_8xx`** (100% ERR-new: 0 such models in vanilla),
**NOT as Treasure events** (so the #1 treasure scan + the bake miss them). 3819 placed assets, 46
models; the old pipeline only mapped 2 (rune 821 / ember 822) and even hardcoded the wrong goods
(800010 "Rune Piece" vs the param's real 998210 "Runic Trace").

**Deterministic chain (any mod, no model→item table):**
```
placed Asset, name "AEG{A}_{B}_xxxx"
  → param row id = A*1000 + B           (e.g. AEG099_821 → 99821; AEG463_860 → 463860)
  → AssetEnvironmentGeometryParam[row].pickUpItemLotParamId   (s32 @ +0xb8, row size 320 = paramdef DetectedSize)
  → ItemLotParam_map[lot] → goods → GoodsName FMG
```
Position = the placed Asset's block-local pos (same transform as treasures). Name-prefix == model
for all 3819 (no substitution among collectibles) → use the PART NAME prefix, no modelIndex read.
Ghidra confirmed: strings `AssetEnvironmentGeometryParam` (er+0x2bb3070) + `ItemLotParam_map`
(er+0x2bb29a8) present; asset-row range bound 99999 (0x1869f) as `CMP/MOV [reg+0x34],0x1869f` in ~20 fns.

**SHIPPED — runtime feature `loot_collectibles` (branch feat/msbe-entity-recover-dummy, commit
59f4bf3, UNPUSHED).** Fully runtime, no bake:
- `msbe_parser` optional `wantAssets` pass → `ParseResult.assets` (AEG name→aegRow + pos).
- `goblin::aeg_pickup_lot(aegRow)` reads `AssetEnvironmentGeometryParam` LIVE (get_param<RawAegRow{320}>,
  u32 @ +0xb8, std::call_once — worker-thread safe).
- `loot_disk`: `DiskCollectible` + `disk_source_enabled()` (treasure OR collectibles share map-dir
  discovery + ONE parse pass; load_disk_treasures gained a collectibles out-param).
- `map_entry_layer::build_disk_collectible_markers`: lot=aeg_pickup_lot→identity live via
  resolve_loot_item_textid→item_marker_category→push_marker(lotType=1); dedup vs treasure `covered`.
- All `lootFromDiskMsb` gates in map_entry_layer/loot_disk → `disk_source_enabled()`.
- Built clang-cl OK, deployed to offline dir. <user> runtime-tests with `loot_collectibles=true`
  (watch `[LOOTDISK] collectibles:` log: emitted / not-a-pickup / dup-of-treasure / unclassified).

**★ MOD-AGNOSTIC via live category fallback (commit e111497).** The only ERR-coupling was the
baked `ITEM_ICONS` classifier (per-item → category+icon); items it missed were dropped
("unclassified"). Added `goblin::classify_item_live(key)`: when ITEM_ICONS misses, derive a GENERIC
category from the LIVE item type — goods → `EquipParamGoods.goodsType` (u8 @ **+0x3e**, row 176):
2→LootCraftingMaterials, 14→LootSmithingStones, 0→LootConsumables, else→CraftingMaterials; equip →
lot cat 2/3/4/5 → Armaments/Armour/Talismans/AshesOfWar. Wired into BOTH disk paths (treasure +
collectible). **Runtime-validated: collectibles 19303 emitted, 0 unclassified, 0 treasure-dup**
(was 119 then 14388 before the per-placement + fallback fixes). Works for any mod's items now.
NOTE scale: 19303 = ALL AEG pickup assets (gather nodes + pots/urns/destructibles), not just the
3819 AEG099_8xx — category toggles + clustering manage the density.

**✅ DENSITY FILTER (2026-06-24, commit 10a473a, UNPUSHED) — already free via the category system.**
Collectible markers route into `g_buckets[cat]` gated by `goblin::ui::category_visible(cat)` (same path
as every marker), and `classify_item_live` maps goodsType→Category, so the existing per-category
`show_*` toggles ALREADY act as a goodsType density filter — NO new filter code was needed.
`show_crafting_materials=false` hides goodsType-2 gather clutter (fireflies/excrement/mosses),
`show_smithing_stones` keeps goodsType-14 stones, `show_consumables` keeps goodsType-0. Toggle live in
the overlay category panel too. This session: refined the classify_item_live catch-all (8→EquipSpirits,
5/17→MagicSorceries, 16/18→MagicIncantations per tools/extract_goods_categories.py; 2/14/0 unchanged,
default still LootCraftingMaterials) so collectibles of those types land in the right toggle instead of
being lumped as crafting mats; documented the show_* filter in the loot_collectibles INI help. The
master on/off is `loot_collectibles` (build-time). A RUNTIME collectible-only on/off (independent of the
shared baked categories) was NOT built — would need per-marker source tagging + render-time filter (the
"Toggle in-game runtime" option, deferred). Bug history: (a) sentinel is -1
not 0 (8c5073c), (b) emit per-placement not per-lot — one lot spans many nodes (94f9c63), (c) live
category fallback (e111497).

**★ Runic Trace ↔ Rune Piece (dug 2026-06-24, commit 20143df).** `AEG099_821` (1164) / `AEG099_822`
(316) ARE the Rune/Ember Piece world locations — already shipped via the BAKED "Reforged - Rune/Ember
Pieces" categories (GEOM-tracked, generate_pieces_massedit). Their `pickUpItemLotParamId` points to the
shared **Runic/Ember TRACE counter** lot (998210/998220 — ONE lot for ALL placements; goods 800011
"Runic Trace" type-1 is given by exactly 1 ItemLotParam_map row, vs the real Rune Piece goods 800010
type-14 given by 194). So the collectible pass emitted 1164+316 DUPLICATE markers mislabeled as the
counter ("too many Runic Trace"). **FIX: skip aegRow 99821/99822 in build_disk_collectible_markers**
(baked categories keep them). Runic Trace = a tally you gain per Rune Piece (200 → Hallowed Avatar);
Rune Pieces are intentionally hidden (intermittent orange glow, often inside breakable vases/boxes) —
ERR wiki err.fandom.com/wiki/Runeforging.

**★★ RE 2026-06-25 — native identity DOES exist (via the ACTION BUTTON); Rune/Ember positions are real.**
(Corrects an earlier wrong read of mine: I looked at `pickUpItemLotParamId` and concluded "no identity, just
curation" — WRONG. <user>: the Rune/Ember **Pieces are the real placed objects (AEG821/822, real positions)**;
the **Runic/Ember Trace is a SHADOW REWARD** — the tally you silently gain on pickup — NOT the object's identity.
Resolving the pickup LOT gives the shadow reward → the wrong "Runic Trace" label.) **The native per-object name
is the ACTION BUTTON:** `asset → pickUpActionButtonParamId (7829/7830) → ActionButtonParam.textId (2829/2830) →
ActionButtonText.fmg` = **"Collect rune piece" / "Collect ember piece"** (EN; "收集卢恩碎片"/"收集余烬碎片" ZH).
Verified via tools/_probe_actionbtn.py (throwaway). This chain is GENERIC + mod-agnostic — and is arguably a
BETTER identity source than `pickUpItemLotParamId→goods` for ALL collectibles (it gives the OBJECT, sidestepping
shadow-reward lots). So labeling AEG821=Rune Piece is NOT fragile curation — the game itself names it so.
**Migration is viable for IDENTITY + POSITION** (collectible pass already reads AEG821/822, currently skips them):
place them, name via the action-button text (or model→800010/850010), and the existing finalize geom-dedup drops
the baked twins. **One OPEN blocker = graying:** the pieces have NO per-piece event flag (param has no
getItemFlagId; EMEVD collection is COUNTER-based — template `$Event(1042622100)` in common.emevd, ~430× per
region, watches the pickup SpEffect → `IncrementEventValue` Trace tally + threshold-milestone flags only, no
AwardItemLot/per-piece flag). Baked pieces gray via GEOM/SFX-tracking (goblin_collected, keyed by object_name/
geom_slot, today BAKE-driven — does NOT see runtime markers). So a clean runtime migration needs the geom-tracking
extended to runtime markers (carry AEG object_name→geom_slot into the disk pass) — an IMPLEMENTATION effort, NOT a
data gap. That same work frees the residual 909 Material Nodes too. Generic-path recon
(CSWorldMapPointMan/MapItemMan/CSMsbPointWorldMapPoint) = dead ends for whole-world map content (live
WorldMapPointParam only 740 rows — boss+nav; loaded-tile only; MSB Points=nav) — but the ACTION BUTTON is the
generic IDENTITY win. See [[nobake-coverage-scoreboard]], [[darkscript3-emevd-decompile]].

**✅ FIXED 2026-06-24 (branch feat/msbe-entity-recover-dummy, built+deployed, UNPUSHED) — tooltip names
for runtime fallback items.** Root cause: `setup_messages` preloads FMG names into PlaceName ONLY for
items the BAKE references (`goods_ids_needed` collected by walking MAP_ENTRIES at init); disk collectibles
resolve identity LIVE from ItemLotParam on the build worker → their goods name was never copied →
`lookup_text_utf8(key)` returned "" → no tooltip label. **The fix mechanism ALREADY EXISTED**: the
`config::liveLootLabels` block in `goblin_messages.cpp` (~line 731) calls `copy_fmg_all_layered` to pull
the WHOLE item-name space (goods/weapon/protector/accessory/gem) into PlaceName at the same offset
encoding markers use (goods → id+500000000 = exactly the `resolve_loot_item_textid` key). But
`liveLootLabels` defaults **false on ERR** (true only on Vanilla via MFG_PROFILE_VANILLA), so it never
ran for the collectibles user. **FIX = one-line gate change**: run the full preload when
`liveLootLabels || lootFromDiskMsb || lootCollectibles` (any disk loot source needs it — they can point a
marker at any item id, AEG gather goods or randomized equip in a pot). All families preloaded (the live
category fallback can class a collectible as armament/armour/talisman/ash, not just goods). Encoding
verified: collectible marker textId1 = `resolve_loot_item_textid(lot,1,-1)` → tooltip `lookup_text_utf8`
hits the same key copy_fmg_all_layered wrote. Built clang-cl + deployed. <user> runtime-tests
(`loot_collectibles=true`, hover a Gaseous Stone / gather node → real name now shows).

**⚠️ ARCHITECTURE DEBT (why all the blacklists/filters; user raised 2026-06-24).** The `_8xx`-only
scope (commit 17b0dc8), the rune/ember skip, the treasure-lot dedup, the lotId-coverage replace — all
are RECONCILIATION between FOUR overlapping loot sources that coexist mid-migration: (1) baked
MAP_ENTRIES (curated items_database), (2) runtime disk treasures (#1, replaces baked lots), (3) runtime
disk collectibles (this), (4) live category fallback. There is NO clean "is this a noteworthy
collectible?" flag in game data (`pickUpItemLotParamId` marks EVERY pickable incl. ~16k breakable
pots), so we filter by heuristic. **Clean end-state = ONE source of truth + ONE curation policy:**
either (A) bake stays the curated authority for what/where + runtime only overlays live identity, or
(B) a single unified runtime pipeline that ports the offline pipeline's curation (unreachable/clutter
filter, dedup, category, notability) — the true no-bake goal. Can't do cleanly until the loot pipeline
is fully one or the other; today's hacks are migration scaffolding. See [[handoff-loot-from-real-files]].

**★ ARCH-DEBT STEP 1 DONE 2026-06-25 (commit 116e07e, UNPUSHED) — provenance tag.** Reframed #6: a
literal "one source" is IMPOSSIBLE — enemy drops (~25608, on ChrIns not MSB), EMEVD grants (~711, mostly
unplaceable per #2), curation residual (position-clip needs vanilla geometry), packed maps (no archive
reader), and ALL non-loot map features (quest NPC/imp statues/paintings/etc., curated in MAP_ENTRIES) are
NOT runtime-derivable. So the real goal = make the source BOUNDARY explicit + shed only the proven-covered
slice, NOT kill the bake. Step 1 = provenance: the pipeline already computes `source` (extract_all_items:
'treasure'/'enemy'; +emevd) but never emitted it. Added `MapEntry.loot_source` (enum LootSource
Unknown/Treasure/Enemy/Emevd, TRAILING field so old generated .cpp zero-inits to Unknown = INERT until
regen); generate_loot_massedit writes source as the 3rd linkage element; generate_data maps it+emits;
build_buckets_impl got a PROVENANCE GUARD (only drop Treasure|Unknown rows on a disk lotId match → never
evict Enemy/Emevd on a lotId collision). Built inert-safe (old data all Unknown → behaviour identical).
**ACTIVATION = a data-pipeline regen** (populates loot_source); that then unlocks #11 (shed only the
source==Treasure slice from the 18MB items_database.json, keep enemy/emevd/curated/non-loot baked). The
docs file docs/loot_source_architecture.md is the planned ADR (NOT yet written — was deferred for the impl).
**★ ACTIVATED for ERR 2026-06-25 (commit c31b54b).** Regen'd the ERR profile → loot_source populated:
Treasure 3639 / Enemy 119 / Emevd 529 / Unknown 4366 (non-lot map features w/o lotId). MAP_ENTRY_COUNT
unchanged (8653); guard now LIVE for ERR (Treasure replaceable, Enemy/Emevd protected). **⚠️ REGEN
GOTCHA:** the regen also re-ran `generate_location_overrides`, which produced 0 entries (vs committed 584)
and WIPED goblin_location_alt.cpp — a PRE-EXISTING non-reproducibility (silent MSBE-read failure in this
env; inputs unchanged; reproduced fine 2026-06-19). UNRELATED to provenance (doesn't read the linkage).
Reverted goblin_location_alt.cpp to preserve data; flagged as a background task. ANY future full regen must
revert that file until the generator is fixed. Other 4 profiles NOT yet regen'd → still all-Unknown (guard
inert there) — fine, they're git-ignored + rebuilt on demand.

**★ DEBAKE-GAP CHARACTERIZED 2026-06-25 (diag commit 72ef36e).** Added `[DEBAKE-GAP]` diag (count of
LootSource::Treasure baked rows whose lot isn't in disk_lots = would be LOST if the treasure slice is
de-baked). Runtime: **328** (326 distinct lots; NOT multi-part; NOT dropped-dummies — recover-later=1).
Cross-ref vs items_database.json (py join on itemLotId) → ROOT CAUSE = **corpse/body item-pickup loot**:
the gap lots are bound to parts AEG099_**630** (166), **090** (83), 990 (31), 600/610/620 (33) — asset
pickups delivering loot via `pickUpItemLotParamId` (SAME mechanism as collectibles) but on models
OUTSIDE the `_8xx` scope. The collectible pass filter `sub=aegRow%1000; if(sub<800||sub>899) skip`
drops them as "clutter" (e.g. AEG099_630 sub=630<800), and the treasure pass misses them (NOT
Events.Treasure). So they fall BETWEEN the two disk passes. Concrete missing items: Fire Monk + Cuckoo
Knight FULL armour sets, Crimson Hood. Concrete: Fire Monk + Cuckoo Knight FULL armour sets, Crimson Hood.
**★ CORRECTION 2026-06-25 (the "recoverable via filter" hypothesis was WRONG — disproven by runtime
diag commit 6b4d801).** Added `[DEBAKE-RECOVER]`: of the 453902 excluded non-_8xx clutter, 15574 ARE
pickups w/ a lot, but **0 flagged, 0 equipment, 0 treasure-placed** → the AEG099_630/090 assets'
`pickUpItemLotParamId` resolves a DIFFERENT lot than the corpse's armour, so widening the collectible
filter does NOT recover them. ROOT (confirmed offline): the 328 gap ≈ the **346 partBucket-NONE treasure
rows** in items_database. The CLEAN treasure path (by_lot over msb.Events.Treasures on LIVE Parts.Assets)
always sets partBucket='live' (3637 of them — these the disk DOES reproduce). The 346 partBucket-NONE are
bound to their asset by an OFFLINE HEURISTIC (the fallback + enrich_fallback_with_emevd.py: lot→entity by
id-prefix/byte-match → partName=entity model, source 'treasure'/'emevd_treasure'), NOT a clean MSB
structure the runtime msbe_parser reads. So they are **NOT cheaply disk-reproducible** — the runtime has
neither the Events.Treasure binding nor a matching pickup lot. **VERDICT: these ~328 must STAY BAKED as a
small curated residual (~9% of the treasure slice).** The prudent no-bake path = de-bake ONLY the
disk-proven partBucket='live' Events.Treasure slice, keep the partBucket-NONE residual baked (provenance
could encode it: live→disk-replaceable Treasure, none→CuratedResidual that stays baked). RE-ing the real
in-game delivery of AEG099_630 corpses to teach the parser is a separate, larger effort. The
[DEBAKE-RECOVER] diag can be removed now (it proved the dead-end); [DEBAKE-GAP] count stays useful.

**✅ "spots-remaining" census fixed 2026-06-25 (commit 7601e79, UNPUSHED) — NO RE needed.** User wanted
an accurate "spots restants" counter (#5). Investigation verdict: **there is NO native global "spots
remaining" structure to read.** The mod already reads the 3 native collected stores — EventFlagMan
(orp_flag_set/read_event_flag, EVENT_FLAG_MAN_SLOT + IsEventFlag = per-item persistent obtain flag),
GeomFlagSaveDataManager (geom_flag_slot, per-TILE geom flags, count@+0x189d0 = tiles not items),
CSWorldGeomMan (live alive flags +0x263/+0x26B) — and the census is a mod-side aggregation, not a native
counter. One-time spots have a persistent flag (census already correct); RESPAWNING gather nodes
(isEnableRepick, getItemFlagId==0) have NO permanent "done" state by design (they regrow at rest), so
nothing native can be read for them. The bug: refresh_overlay_census (map_entry_layer.cpp ~518) added
those flag-less nodes to `total` but they could never reach `looted` → "remaining" inflated forever.
FIX (user chose "exclude them"): a respawning node is not a completion spot → drop flag-less lot-backed
markers from total/looted entirely; the badge now counts only completable (persistent-flag) spots. Logs
`[CENSUS] excluded N respawning` per category. Map graying UNCHANGED (respawners still show/hide on live
geom state). **The OTHER possible "native counter" = inventory item QUANTITY (Runic Trace count etc.) via
GameDataMan→PlayerGameData→EquipInventoryData — the mod does NOT read GameDataMan; that's a separate
feature ("how many you own", low-risk standard RE) if ever wanted, NOT what #5 turned out to be.**

**Hide-on-pickup:** via the standard live loot flag (push_marker lotType=1 → resolve_loot_flag →
ItemLotParam getItemFlagId), NOT the GEOF piece-tracking (`goblin_collected.cpp`, which is built from
baked MAP_ENTRIES and does NOT see runtime markers). Respawning nodes (`isEnableRepick`, 74/285) have
no persistent flag → stay shown (correct). Offline cross-check: `tools/extract_err_collectibles.py`
(positions) + `tools/extract_aeg099_mapping.py` (model→item, 285 rows). See [[handoff-loot-from-real-files]],
[[msbe-dummyasset-filter]].
