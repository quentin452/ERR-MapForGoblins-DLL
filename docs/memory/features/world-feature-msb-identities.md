---
name: world-feature-msb-identities
description: "The MSB AEG-model / Region / param identity of every World feature (Stakes, Spirit Springs, Summoning, Imp, NPCs, …) — the no-bake source for the ~1300-baked backlog. Already datamined in the tools/generate_*.py the bake came from."
metadata: 
  node_type: memory
  type: project
---

**The per-feature "which MSB model/Region = this feature" RE is ALREADY DONE** — it's encoded
in the `tools/generate_*.py` scripts the static bake was built from. To migrate a World feature
off the bake ([[nobake-coverage-scoreboard]] item 3, ~1300 baked, no object_name), port the
matching generator's selector into a C++ runtime disk pass over the resident/disk MSBs
([[runtime-msb-resident-plan]]). The C++ parser already enumerates **Assets** (name→aegRow→pos,
`wantAssets`) and **Enemies** (NPCParamID, EntityID, pos); it does NOT yet parse **Regions** or
join **params** by position.

**Identity table (feature → MSB source → generator):**
- **Stakes of Marika** = Parts.Assets `AEG099_060`. No flag. → **DONE + RUNTIME-VALIDATED 2026-06-25**
  (`build_disk_stakes_markers`, config `world_features_from_disk`, branch feat/world-features-stakes).
  Disk = **219** world-distinct stakes; the bake's "439" was LOD-phantom-inflated (see ⚠ below). Finalize
  = CATEGORY-WIPE (drop ALL baked when disk placed ≥1), not positional dedup.
- **Imp/Seal Statues** = Parts.Assets `AEG027_078` + `AEG027_079` (+ EntityID). `generate_imp_statues.py`.
- **Hero Tomb Statues** = Parts.Assets `AEG099_055`; interactive = has EntityID, decoration = none. `generate_hero_tomb_statues.py`.
- **Spirit Springs** (71) = Regions `MountJumps` + `LockedMountJumps` (LAUNCH pts; the *Falls dups
  are LANDING, skip) + Others region name contains `FakeSpiritSpringJump` + DLC Parts.Assets
  `AEG463_200`. `generate_spirit_springs.py`. Needs Region parsing.
- **Spiritspring Hawks** = Parts.Enemies `c4210`, EntityID%10000 ∈ {980,971}; clearedEventFlagId =
  EntityID (hawk kill = spring unlock). Same generator.
- **Summoning Pools** (245) = `SignPuddleParam` (per row: flag@unknown_0x3c, map_ref@0x28,
  pos x/y/z @ 0x2c/0x30/0x34); overworld tiles resolved by matching the pos to MSB Parts.Assets
  `AEG099_015`. `generate_summoning_pools.py`. Needs the SignPuddleParam offsets pinned live
  (see [[re-offset-validation]]) + a position-join to AEG099_015 assets.
- **Quest NPCs** (344) = live NpcParam (quest npc-id set) ∩ Parts.Enemies whose NPCParamID is in
  that set AND EntityID>0; teamType filter, exclude model `c1000`. `generate_quest_npcs.py`.
- **Hostile NPCs** = Parts.Enemies + NpcParam, EntityID>0, invader teams; exclude bloodfiends
  c4280, dungeon battlemages c4300_*_28, scarabs c4190/91/92. `generate_hostile_npcs.py`.
- **Paintings** = EMEVD painting-template events (flags 580000–580199) + MSB positions.
  `generate_paintings.py`. Needs EMEVD parse (parser does emevd) + position join.
- **Seal Puzzles** = `data/seal_puzzles.json`; activation flag = CSEmkEvent template 90006050/51
  param[1]; ActionButtonText 9503 "Examine seal" / 9520 "Light flame". `generate_seal_puzzles.py`.
- **Kindling Spirits** = `data/kindling_spirits.json` (5 SFX-regions in m60_45_37_00; per-spirit
  state is RAM/SFX-only, no event flag). `generate_kindling_spirits_massedit.py`.
- **Maps** = `items_database.json` Map: items (loot-like, not an MSB feature). `generate_maps.py`.

**✅ DONE 2026-06-25 — data-driven refactor SHIPPED (builds clean, awaiting runtime test).** The
hardcoded `kStakeAeg`/`build_disk_stakes_markers` is GONE. New shape: editorial table
`tools/world_feature_assets.py` (rows = `{aeg_row, model, category, text_id, entity_required,
category_wipe}`) → `tools/generate_world_feature_models.py` emits
`src/generated/goblin_world_feature_models.{hpp,cpp}` (`WORLD_FEATURE_MODELS[]`, profile-indep,
wired into build_pipeline.py + CMakeLists) → ONE generic runtime pass
`build_disk_world_feature_markers` (map_entry_layer.cpp) looks each disk asset's aegRow up + emits.
Parser now exposes **Asset.entityId** (msbe_parser read @part+0x60/+0x00, plumbed through
DiskCollectible) for the interactive-vs-decoration split. **Finalize is TWO modes via the
`category_wipe` flag** (the key insight): DEDICATED category (Stakes, Imp) → wipe ALL baked;
SHARED category (Hero's Tomb shares WorldInteractables with Seal Puzzles!) → cell-dedup only, so
the puzzles stay baked. 4 rows seeded: Stakes(99060,wipe,no-entity), Imp(27078+27079,wipe,entity),
Hero Tomb(99055,cell,entity). Add a new asset feature = ONE row.

**RUNTIME-VALIDATED 2026-06-25 (1st pass, before graying):** [COVERAGE] confirmed Stakes baked0/disk219
(=predicted; wiped 439 LOD-phantom baked), Imp baked0/disk39, Interactables baked88/disk14 (=Hero Tomb
off-baked, the 88 Seal Puzzles correctly KEPT by cell-dedup). 0 parse fail, 0 warn. cat indices in log:
56=Imp 61=Stakes 64=Interactables (= enum line−12).

**✅ GRAYING RESTORED 2026-06-25 (option a, builds clean, awaiting runtime) — flag_rule field added.**
The 1st pass dropped the bake's textDisableFlagId (Imp seals + Hero Tomb stopped hiding when used).
Fix: table gained `flag_rule` enum {None, ImpSeal, HeroTombEmevd} → the pass sets d.textDisableFlagId1
(push_marker's collected_flag path works for lotId=0 — resolve_loot_flag returns baked_flag directly).
• **ImpSeal** = pure runtime arithmetic, no bake: rejects non-seal suffixes {570,575,565,611} (this also
  FIXES the old +3 over-include → Imp now 36=bake), flag = tile_base(area,gx,gz)+suffix (m60/m61 tiled
  differently, mirrors generate_imp_statues.py), textId 565→Imbued(500008186) else Stonesword(500008000).
• **HeroTombEmevd** = new `parse_emevd_flag_awards` (msbe_parser, template 90005683 entity@+12 flag@+20)
  → `load_emevd_world_feature_flags()` (loot_disk, scans event\ via resolve_event_dir) → entityId→flag
  map joined in the pass. Only scans EMEVD if a HeroTombEmevd row exists (Imp/Stakes skip the ~500-file read).
Asset.entityId now parsed for ALL assets (msbe_parser @+0x60).

**✅ RUNTIME-VALIDATED IN-GAME 2026-06-25 (graying + GED):** [COVERAGE-CENSUS] 13:51 in-game pass:
Imp drawn37/flagged37, Interactables drawn102/flagged102 (88 puzzles + 14 Hero Tomb all flagged),
Stakes nonloot219 (never grays, correct). EMEVD flags=16 entity→flag, 0 interactive-without-flag.
Graying CONFIRMED working. **GameEditionDisable filter (commit 27e9dcf, parser +0x44 pinned via
tools/probe_gameedition_offset.py 6612/6612 incl 30 pos) dropped the 1 disabled Imp (38→37).** The 3
disk-vs-bake Imp extras fully explained: 1 bad-suffix {≠570/575/565/611} (rule-rejected), 1
GameEditionDisable=1 (now filtered), 2 in unreachable.py curated list (m60_38_52, m60_47_40 — ERR
clipped them under terrain; editorial + ERR-version-specific, NOT runtime-derivable w/o vanilla MSBs,
LEFT as-is). GED filter benefits ALL asset features. ⚠ title-screen build runs prematurely (params not
ready → 0 loot, BOSSLIVE-not-readable) but RE-KICKS in-game (the 13:51 pass = full: loot3404 enemy123
emevd550 boss217) — harmless, the late build supersedes.

**TASK-3 ANALYSIS (graying for live-fallback / ImGui-vs-real-map count, 2026-06-25): the world-feature
"derive-flag-live" trick does NOT transfer to loot.** Census proves every Loot/Equip/Magic/Key category
has flagged+respawn = drawn (zero unexplained nonloot) — lot-backed items already gray via
resolve_loot_flag(lotId,lotType) reading the live ItemLotParam getItemFlagId. The 186 live-classified
(classify_item_live) items (Armaments live-cls78, Crafting100, Smith2, Consum6) are lot-backed too →
they gray fine. The flag-less nonloot rows are intentional (Stakes/QuestNPC/SpiritSprings = no completion
state) or GEOF-geom-tracked (Rune/Ember Pieces, Material Nodes — separate gray path, not event flags).
So ImGui≠real-map count is NOT a graying bug — it's the disk+live pass finding MORE loot than the native
WorldMapPointParam injection shows (overlay more complete than native). NEXT: needs user's SPECIFIC
symptom (which category, ImGui count vs real-map count) to know if there's anything to fix.

**★★ ORIGINAL PLAN (now done, kept for context) — make it DATA-DRIVEN, not per-feature hardcode.**
Before adding Imp/Hero Tomb, refactor the hardcoded `kStakeAeg = 99060` (build_disk_stakes_markers) into
a GENERATED `model→category` table so adding an asset-model World feature = ONE data line, zero C++.
Design: a committed declarative source of truth (e.g. `tools/world_feature_assets.py`/.json) =
`{AEG099_060: (WorldStakesOfMarika, textId 900301540, entity_required?), AEG027_078/079: (WorldImpStatues,
…), AEG099_055: (WorldHeroTomb…), AEG463_200: (WorldSpiritSprings, …)}` → pipeline emits
`src/generated/goblin_world_feature_models.cpp` (`{aegRow, category, textId, entity_required}`) → ONE
generic runtime pass replaces build_disk_stakes_markers (lookup each disk asset's aegRow, emit + per-cat
category-wipe). SCOPE: asset-model features ONLY (Stakes/Imp/HeroTomb/SpiritSpringDLCpart); Region/param/
EMEVD/enemy features keep bespoke passes. PARSER PREREQ: Imp/Hero Tomb split interactive (has EntityID) vs
decoration (none) → extend msbe_parser to expose the **Asset** EntityID (today only Treasure/Enemy read it).
Then Imp + Hero Tomb become data rows, not functions. (No live game param gives the MFG category for these —
stakes/imp have no pickUpItemLotParamId — so a small editorial map is unavoidable, but ONE table covers all.)

**✅ SUMMONING POOLS DONE + RUNTIME-VALIDATED + MERGED 2026-06-25 (master 0f0f93b): 246 pools
live, 0 dedup / 0 unresolved, flagged 246/246, baked 1419→1174.** Bespoke LIVE-param pass build_live_summoning_pools
(map_entry_layer.cpp) — NOT the asset table (param feature). Reads SignPuddleParam live via
from::params::get_param<SignPuddleRow> (minimal raw-offset struct). **SignPuddleParam offsets PINNED — ⚠ re-offset-validation
trap hit: the paramdef field NAMES (unknown_0x28…) are LABELS not byte offsets. REAL offsets
(validated vs raw-serialized row, rid 670099 = int 45 @+0x10, pos (4.69,1.12,-25.95) @+0x14;
tools/probe_signpuddle_offsets.py) are a uniform 0x18 LOWER: mapRef@0x10(int), posX@0x14
posY@0x18 posZ@0x1c(float). The GRAYING FLAG = the ROW ID itself (670099..670980, EMEVD sets it
on unlock), NOT the 0x3c field. First runtime test with the wrong 0x28 offsets gave 42 pools
(201 over-deduped); offline sim of the pass logic = 246 (≈bake 245), so logic was right, read
was off. Fixed commit 26c1eb3.** Dungeon pools (mapRef<100000, 168 of them) decode
tile = area(mapRef%256)+block(mapRef//256→gx, skip 60/61); overworld (mapRef>=100000, 78) position-
join to nearest AEG099_015 asset <10u (rides the disk asset enum). Dedup per-context dup rows within
3u/tile. textId1=900301690. Source::Live, category-wipe. ⚠ DEPLOY BLOCKED: game was running (DLL
locked) — `Copy-Item build-clang\MapForGoblins.dll <offline>` after ER closes. Verify in log:
"[LOOTDISK] world features: N Summoning Pools from live SignPuddleParam" (expect ~245) + scoreboard
WorldSummoningPools baked→live. Needs in-game build (live param) — title-screen pass defers it.

**✅ GREAT RUNES DONE + RUNTIME-VALIDATED IN-GAME + MERGED 2026-06-25 (master 58c0040): 6 off-baked
LIVE, baked 989, 0 boss-not-live, flagged 6/6.** No MSB/disk source — position AND graying come LIVE
from the matching boss marker (build_live_bosses → WorldMapPointParam), joined by clearedEventFlagId.
build_great_rune_markers (map_entry_layer, runs AFTER build_live_bosses, gated worldFeaturesFromDisk)
scans g_buckets[WorldBosses] for marker.cleared_flag==X → reuses its raw pos, emits KeyGreatRunes
Source::Live (textId1=500M+rune_id, gray on cleared_flag). category-wipe (cat 11). **RE finding: the
rune→boss link IS derivable, NOT a hidden structure — RPM/Ghidra unneeded.** EquipParamGoods
**goodsType==15 identifies EXACTLY the 6 runes** (191-196, zero false-pos); each rune's GoodsName shares
its demigod proper noun with the boss name; lone collision (Radahn: Starscourge area60 vs DLC Consort
area20) resolves by "great runes are base-game" → exclude DLC_AREAS (the principled rule, NOT "prefer
overworld" which worked by accident). Disk/EMEVD CAN'T link them: rune lots (34XXX) are emevd_treasure in
m34 internal maps with m34-LOCAL flags, getItemFlagId=goods id (not a real flag), no EMEVD event joins a
boss-death flag to a rune lot. The 6 {rune_id→clearedEventFlagId} pairs: 191→510010 192→510300(Starscourge)
193→510040 194→510220 195→510120 196→510200 (cleared = the WMP boss-cleared flag, set on death, both join
key AND graying). Chose a documented `constexpr` over Python-gen or runtime name-match: runtime match buts
on build-order (DLL expanded-FMG built by setup_messages AFTER the marker pass → lookup_text misses) +
locale fragility; TODO in code for the fully-live alternative if ER ever adds a great-rune demigod
(unlikely). — Campaign tally: baked ~1908→989. 🔴 baked-only left: Material Nodes 11 (GEOF), Kindling 5
(data-file, no event flag), Ammo residual 3 (debake-gap) (+ QuestNPC 344 SKIP).

**✅ GESTURES DONE + RUNTIME-VALIDATED IN-GAME + MERGED 2026-06-25 (master 814b0a6): 7 off-baked,
baked 995, EXACT match + flagged 7/7, 0 no-position, 0 nameless.** Same EMEVD-entity-join pattern as
Paintings + ONE live-param read. A gesture = an MSB **type-13 Asset** (all 7 probed: AEG099_610/600/990/
090, AEG463_625) referenced by common template **90005570** (ER gesture-spawn): args (flag@idx2,
gestureParam@idx3, entity@idx4). New `parse_emevd_gestures` (msbe_parser) returns GestureRef{entity,flag,
gestureParam}; harvested in the SAME ~517-file scan via `load_emevd_world_feature_flags(paintings_out,
gestures_out)` (3rd out-param, no extra read). `build_disk_gesture_markers` joins entity→asset pos +
reads **GestureParam.itemId LIVE** (`from::params::get_param<GestureParamRow>`) → textId1=500M+itemId
(GoodsName). textDisableFlagId1=flag. **GestureParam.itemId @ +0x04 PINNED EMPIRICALLY** (re-offset-
validation: row 0 dataOffset 0x598 → raw `00 00 00 00 | 28 23 00 00`=0x2328=9000=itemId; static_assert
added; byte0=bitfield+3 reserve then s32). Dedup by entity. Dedicated cat (LootGestures=37) → category-
wipe. ⚠ live read needs in-game build (title-screen → nameless); 57 live rows confirmed in-game. census=6
(flag 60824 shared by 2 markers, badge dedups). — Campaign tally: baked ~1908→995. 🔴 baked-only left:
Material Nodes 11 (GEOF), Great Runes 6, Kindling 5 (data-file), Ammo 3 (+ QuestNPC 344 SKIP).

**✅ PAINTINGS DONE + RUNTIME-VALIDATED IN-GAME + MERGED 2026-06-25 (master aa5ca37): 11 off-baked,
baked 1002, EXACT match to bake + flagged 11/11, 0 no-position.** A painting = an MSB part REFERENCED
by an EMEVD painting-collection event (flag 580000-580199): the probe (deleted) showed **4 are Asset
frames (AEG099_386/388/389/391), 7 are ghost-painter Enemies (c4300 / DLC c6320)** — so the join needs
BOTH disk_collectibles AND disk_enemies (both already carry EntityID). **KEY: detection is by FLAG
RANGE, not a template table** — the DLC paintings each use a UNIQUE map-specific template id (2045432550,
2046452550, 2053392300) a fixed-template list can't catch. Two arg shapes mirror generate_paintings.py:
template 90005632 → flag@idx2/entity@idx3; else flag@idx3 (if in range)/entity@idx4. New
`parse_emevd_paintings` (msbe_parser, range-detected, returns entity→flag); harvested in the SAME
~517-file event scan as Hero-Tomb/Hostile flags via `load_emevd_world_feature_flags(paintings_out)` (no
2nd read). `build_disk_painting_markers` joins entity→pos; textId1 = GoodsName FMG DERIVED from flag
(base: 500000000+8200+(flag-580000)/10; DLC: +2008200+(flag-580100)/10 — SAME value the bake stores → icon
+name identical via push_marker's item_icon_id(name_id) path), textDisableFlagId1 = flag (graying).
Dedup by flag (19 raw events → 11, 8 dup-flag from phase tiles). Dedicated cat → category-wipe (default
for non-asset-table cat, cat 58). [COVERAGE] baked=0 disk=11, [COVERAGE-CENSUS] flagged 11/11. — Campaign
tally: baked ~1908→1002. 🔴 baked-only left: Material Nodes 11 (GEOF), Gestures 7, Great Runes 6,
Kindling 5 (data-file), Ammo 3 (+ QuestNPC 344 SKIP deprecated, Interactables-rest). Paintings was the
LAST EMEVD-RE morceau — what remains is data-files + GEOF.

**✅ MAPS DONE + MERGED 2026-06-25 (master 1a36eeb): 24 (disk 23 + 1 baked Altus EMEVD), baked
1013.** Region map fragments = goods pickups placed as MSB treasures. build_disk_maps_markers scans
disk treasures, resolves the lot, routes map-goods → WorldMaps via goblin::goods_is_map = EquipParam
Goods **sortGroupId u8@+0x72 ∈ {190 base,191 DLC}** (⚠ NOT s16 — 1-byte field, reading 2 bytes folds
in the 0x73 bitfield; offset right but TYPE was the bug, cost a build). goods key = resolve_loot_item_
textid → +500M encoding, gid=key-500M. push_marker(lot,1) gives name+icon+flag live. WorldMaps
finalize = CELL-dedup (1 EMEVD-only Altus map has no treasure → kept baked). flagged 24/24.

**✅ SPIRITSPRING HAWKS DONE + MERGED 2026-06-25 (master 9db0c40): 14 off-baked, baked 1036, EXACT
match to bake + flagged 14/14.** c4210 disk enemies, EntityID%10000∈{980,971}, model via DiskEnemy
part-name prefix (now carried, SSO), clearedEventFlagId=EntityID (kill=spring unlock). Rides existing
DiskEnemy enum. — Campaign tally: baked ~1908→1036 (Stakes/Imp/HeroTomb/Summoning/HostileNPC/Spirit
Springs/Hawks). 8 🔴 baked-only left: QuestNPC 344 (SKIP, deprecated), Interactables-rest, Maps 24
(data-file), Paintings 11 (EMEVD), MaterialNodes 11 (GEOF), Gestures 7, GreatRunes 6, Kindling 5
(data-file), Ammo 3.

**✅ SPIRIT SPRINGS DONE + MERGED 2026-06-25 (master c566449): 72 off-baked, baked 1050.** Region
parsing shipped (msbe_parser wantRegions). Runtime 72 = 67 regions (48 MountJump + 5 Locked + 14
FakeSpiring) + 5 AEG463_200, 0 dedup; vs bake 71 → +1 = the editorial-unreachable spring (m60_39_43
"to Rune Wolf Forest", ERR moved 110u down; unreachable.py not runtime-ported, like Imp's 2). No
flag (springs don't complete). DATAMINING confirmed, no RPM.

**✅ MSBE REGION LAYOUT RE'd 2026-06-25 (now SHIPPED, branch merged) — DATAMINING, no RPM.** Spirit Springs (71) = ALL disk MSB _00: Regions MountJumps(48)+
LockedMountJumps(5) + Others/FakeSpiritSpringJump(14, name-filter) + Parts.Asset AEG463_200(5) =
72 raw → 71. The parser ALREADY loads the POINT (region) section — line 45 `MODEL,EVENT,POINT,
ROUTE,LAYER,PARTS` → secs[2]=POINT, just add SEC_POINT=2 + iterate. **Region entry layout PINNED
vs SoulsFormats over 651 tiles (tools/probe_region_layout.py): name@+0x00(i64 offset, eio),
subtype@+0x08(i32) MountJumps=46 / LockedMountJumps=54 / Others=-1, position@+0x14(Vec3) — 100%
(MJ 48/48, Locked 5/5, Fake 14/14).** ⚠ NOTE region pos@+0x14 ≠ Parts pos@+0x20 (different
header). NEXT: add Region parsing to msbe_parser (wantRegions, filter type∈{46,54} + Others name
contains FakeSpiritSpringJump) → build_disk_spirit_springs_markers; AEG463_200 = a world_feature_
assets.py row (asset enum already parses it). Hawks(14)=c4210 enemies, rides DiskEnemy, flag=EntityID.

**✅ HOSTILE NPC DONE + MERGED 2026-06-25 (master 2b81580): 50 invaders, off-baked, baked
1121.** Bespoke enemy-join pass build_disk_hostile_npc_markers — placed MSB enemy (entityId>0)
whose LIVE NpcParam = named invader (teamType∈{24,27} ∧ nameId>0; the nameId>0 gate excludes
mobs sharing the team). New live read npc_team_and_name: **teamType@0x133(u8), nameId@0xc(s32)**
pinned via paramdef-layout walk + raw rows + the known itemLot 0x30/0x34 cross-check
(tools/probe_npcparam_offsets.py). Defeat flag from EMEVD 90005792 (entity@+20,flag@+8) folded
into the shared flag-template parser. Dedup by bake raw-position key (NOT projected cell — cell
0.5u wrongly merged 1). category-wipe. **KEY FINDING: the disk's 50 is MORE accurate than the
bake's 53 — the bake double-counts 3 invaders present in BOTH _00 (GED=0) and _10 (its dedup key
includes the tile name → _00 + _10 copies not merged). So _00-only lost NOTHING.** ~23 invaders
have no 90005792 flag → don't gray (no items_database/override fallback ported).

**✅ TILE-COVERAGE TRACKING + the _00-tier verdict 2026-06-25:** runtime logs [COVERAGE-TILES]
(parsed _00 vs skipped non-_00 by LOD suffix); scoreboard renders a Tile-coverage table.
tools/tier_coverage.py audits per-tier UNIQUE content. **VERDICT: the _00-only rule loses ~no
real markers — DON'T loop-all.** Of 964 tiles, 651 are _00. Non-_00: _01(180)/_02(61) = LOD
connect-proxies (the Stakes/Imp 128/256 offset PHANTOM source — looping them REGRESSES assets);
_10(36)/_11(11)/_12(5) = mostly GED-tier DUPLICATES of _00 (only 1 _10 + 6 _12 enemy EIDs are
truly unique, none invaders); _99(20) = lighting. So blanket "parse all tiles" is WRONG (phantoms
+ double-counts); _00-only is correct. The 6 unique _12 enemies are the only real (tiny) gap.

**★ NEXT TARGET (handoff 2026-06-25) — baked-only categories by count (scoreboard):** Quest NPC 344,
Summoning 245, Interactables 102, Spirit Springs 71, Hostile NPC 53, Imp 36, Maps 24, Hawks 14,
Paintings 11, Material Nodes 11, Gestures 7, Great Runes 6, Kindling 5, Ammo 3. **RECOMMENDED ORDER:**
(1) **Imp Statues** (AEG027_078/079, 36) = the easy proven win — clone `build_disk_stakes_markers`
(map_entry_layer.cpp), change the model row + the category, add the EntityID read; ride the same
`world_features_from_disk` flag + asset enumeration + category-wipe finalize. ⚠ its icon is `atlas 30%`
(near the faint threshold — eyeball it / consider enlarging). Then **Hero Tomb** (AEG099_055, part of
Interactables). (2) **Summoning Pools** (245) = biggest REAL impact but needs SignPuddleParam live offset
pinning ([[re-offset-validation]]) + AEG099_015 position join. **SKIP Quest NPC (344)** despite being
biggest — it's the DEPRECATED legacy category (superseded by the in-overlay Quest Browser, show_quest_npc
default off), so off-baking it is low value.

**Difficulty tiers for the C++ runtime port:**
- **EASY** (pure Asset-model scan; parser ready, ride the existing asset enumeration like Stakes):
  Imp/Seal (AEG027_078/079), Hero Tomb (AEG099_055), Spirit-Spring DLC part (AEG463_200).
- **MEDIUM** (Enemy join; parser does enemies, needs npc-id filtering): Spiritspring Hawks (c4210),
  Quest NPCs, Hostile NPCs.
- **MEDIUM** (needs new **Region** parsing in msbe_parser): Spirit Springs main.
- **HARDER**: Summoning Pools (pin SignPuddleParam offsets live + AEG099_015 join), Paintings (EMEVD).
- **DATA-FILE, not MSB** (lower no-bake-from-MSB priority): Seal Puzzles, Kindling, Maps.

**⚠ "Category X markers don't show" is usually ICON SIZE, not a bug — check that FIRST.** When the
Stakes migration looked broken (nothing on the map even with the category on), it was NOT a code /
projection / visibility bug — **the atlas-CPU category icon (iconId 405) is just small/faint and got
lost on the busy map**. Confirmed 2026-06-25: bumping `overlay_icon_scale` (ini, or the F1 "Scale for
category marker ICONS" slider) made the stakes appear. Why it's easy to misdiagnose: the categories
that DO show (loot, bosses, graces) use **native-GPU icons** (ItemIconProvider / MENU_MAP_ERR_Boss /
grace sprite), while World features fall through to the **baked atlas CPU cell** (`category_icon_key` →
ICON_CELLS), which can be a tiny glyph. The render path is fine — `icons.resolve` succeeds, `AddImage`
draws the small cell. Decisive diagnostic technique (reusable): (1) draw magenta `AddCircleFilled` discs
at each `would_draw` marker's screen pos — confirms projection/visibility independently of the icon;
(2) draw the raw atlas cell + a known-good reference (graces) at a fixed screen corner at 64px —
isolates icon-cell-broken vs atlas-broken vs icon-too-small. The gates (`category_visible` /
`section_visible` / `seed_runtime_gates`, called once at boot) were all fine. Possible real fix for
later: enlarge small World-feature atlas icons in `generate_overlay_icons.py` (the PLATE_ZOOM path) or a
per-category default scale, so they don't need the global `overlay_icon_scale`. **✅ DONE: the
`nobake_scoreboard.md` now has an `icon` column** (commit 4b14c5d, branch feat/scoreboard-census-flag-
coverage): per category = `symbol` (GPU) / `atlas N%` (CPU cell coverage; trailing ⚠ if <25% faint) /
`circle` / `none`. Stakes shows `atlas 19% ⚠` — the ONLY ⚠ category, would have flagged this instantly.
Coverage decoded offline from the embedded ATLAS_PNG (Pillow optional → cached in tools/atlas_coverage.json
so py 3.14 no-PIL runs keep the %).

**⚠ The bake is NOT a reliable per-feature oracle — trust the `_00` world-cell count.** `generate_stakes.py`
(and likely the other asset-scan generators) scans EVERY tile incl. LOD-coarse `_02` (which proxy objects at
a 128/256 offset — parse `_00` only) AND deduped on `(area, x, z)` block-local with **no gx,gz** → the baked
rows were inflated with offset phantoms (Stakes: 226 AEG099_060 in `_00` vs 250 in non-`_00`; bake=439 but
disk world-distinct=219). So the finalize dedup for a feature should be **CATEGORY-WIPE** (drop all baked
when the disk pass placed ≥1, like live bosses), NOT a positional dedup (which leaves the phantoms). Verify
each new feature the same way (`_00` raw count vs the bake) before trusting the bake's number.

**Pattern to replicate (from Stakes):** request the asset enumeration when the feature flag is on
(`wantAssets = lootCollectibles || worldFeaturesFromDisk`), a `build_disk_<feature>_markers` that
filters `disk_collectibles` by aegRow and `push_marker(..., Source::DiskMSB)`, dedup by 0.5u Cell,
and a finalize position-keyed drop of the baked twin (KEEPS baked rows the disk missed). textId1 =
the feature's tutorial text for the tooltip (Stakes 900301540); icon/category from the category.
