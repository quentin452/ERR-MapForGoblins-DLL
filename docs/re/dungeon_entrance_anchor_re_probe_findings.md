# RE probe — dungeon-entrance anchor source (Slice 3 gate)

**Date:** 2026-07-23 (live, in-world, ERR install). Gates Slice 3 of
[dungeon_entrance_fallback_anchor_plan](../plans/dungeon_entrance_fallback_anchor_plan.md).
Goal: find a mod-agnostic (overworld-frame) source that yields, for a dungeon area `legacy_fold`
declines, its overworld ENTRANCE position — validated against known dungeons' `ConvRow.dst`.

## Ground truth — `ConvRow.dst` entrances (new `fold_probe` verb)

`fold_probe <area> <gx> <gz> [px] [pz]` runs `legacy_fold::fold` and reports `matched` + the folded dst
block + `ent=(ent_x,ent_z)` (= the overworld entrance base point, `ConvRow.dst` world). Known catacombs:

| area | matched | entrance `ent(x,z)` |
|------|---------|---------------------|
| 30 | 1 | (10913, 8522) |
| 31 | 1 | (11008, 9472) |
| 32 | 1 | (11044, 8380) |
| 39 | 1 | (12746, 13046) |
| 34 | 0 (fold(34,0,0) unmatched — needs its sub-grid; markers still project via a chained row) |
| **45** | **0**, `ent=(0,0)` — **declined** (the bug area) |

These are the target values any entrance source must reproduce.

## Candidate sources — what FAILED (all live-checked)

- **cat=55 `WorldLegacyDungeon` markers** — `srcArea` is the dungeon's OWN area (10/30/31/32/34…), i.e.
  they are **folded** WMP rows. An area-45 legacy-dungeon marker would be `srcArea=45` → fold declines →
  origin, same as everything else. No free overworld anchor.
- **cat=54 `WorldDungeon` entrance icons** — 713 rows at `srcArea=60` (true overworld) DO sit near each
  fold entrance (e.g. `w(10868,8562)`,`w(10955,8310)` around a30/a32). **BUT every one carries the SAME
  generic `name_id=500800010`** ("dungeon entrance" PlaceName) — no area encoding. And `WORLD_MAP_POINT_
  PARAM_ST` has **no target-map/area/entity field** (only `eventFlagId`). So the icon gives a POSITION
  but no LINK to the dungeon area it opens.
- **Grace** — projects through the same failing fold (`grace_layer.cpp:39`); area-45 grace is area-45
  frame → useless as an anchor.

## Conclusion

The overworld **position** source exists (cat=54 entrance icons at `srcArea=60`), but **no param field
links an overworld entrance to a dungeon area**. The only source of that link is the **EMEVD warp event**
(the overworld trigger asset → `WarpPlayer`/travel to `m45`). So Slice 3 must decode EMEVD warps — no
param shortcut. (Confirms the plan's premise; the cat=55/cat=54 shortcuts are now ruled out with data.)

## Next concrete RE step (the actual warp decode)

For a KNOWN catacomb (area 30, entrance `(10913,8522)`): find its overworld→interior warp and read
(trigger entity, destination map). Two paths:
1. **Extend `parse_emevd`** (`msbe_parser.cpp:485`) to capture the warp instruction's dest-map + trigger
   (the current portal parse `parse_emevd_portal_gates:653` reads gate entity only, drops the dest map).
   Needs the warp template/instruction id + arg layout — decode from a real EMEVD.
2. **Offline decompile** one overworld-tile EMEVD (DarkScript3, see
   `docs/memory/tooling/darkscript3-emevd-decompile.md`) to READ the m30 warp by hand first, learn the
   template + args, then implement (1).
**Validate:** the warp trigger's overworld position (from the overworld MSB asset, joined by entity id)
must ≈ `(10913,8522)` (a30's `ConvRow.dst`). If it matches on 2–3 known dungeons, the method is proven →
apply to `m45` (which has no `ConvRow`) to seed `entrance_anchor`.

Related: `fold_probe` / `entrance_anchor` RPC verbs (`goblin_debug_rpc.cpp`), `goblin_legacy_fold.cpp`
(`ent_x/ent_z`), `msbe_parser.cpp` (EMEVD parse to extend).

## EMEVD warp decode — round 1 (live, 2026-07-23, new `vmap emevd` probe)

New probe `vmap emevd <mapName> [needle]` (dumps an EMEVD's bank-2000 InitializeEvent inits: containing
event, invoked template, raw arg words; needle filters by entity/template/arg). Data:

- **m30_04_00_00** (a catacomb): 41 inits / 19 templates — ALL reference the catacomb's OWN entities
  (`3004XXXX`): boss-fog family `9005800/01/11/22` (`30040800`=boss,`30041800`=fog,…), grace
  `90005650/651` (`30040540/30041540`). **No overworld / cross-map reference.**
- **m60_43_33_00** (overworld tile at a30's fold entrance ~`(10913,8522)`): 41 inits / 21 templates. The
  transition-looking family `90005720-724` + `90005600`/`900005610` reference only **tile-local** entities
  (`1043330290…` = m60 43_33 local) + a float arg (`1084227584`=5.0f radius). **No `30XXXXXX` dungeon
  dest** — so the overworld→catacomb warp is NOT a bank-2000 init in this tile.

**Conclusion (round 1):** the overworld→dungeon link is not in bank-2000 init args. Two remaining sources
to test:
1. **Direct `WarpPlayer` instruction (bank 2003)** — the init-dump only sees bank-2000; extend the probe
   to dump ALL instructions (any bank) and find the warp instruction + its dest-map arg. (Catacombs are
   loading-screen warps → likely an EMEVD `WarpPlayer`.)
2. **MSB `ConnectCollision` (part type 5)** — SEAMLESS dungeons (Stormveil↔overworld) connect via a
   ConnectCollision part carrying the target `MapID[4]` (area/block/cc/dd) + a position = a DIRECT
   (overworld pos → target map) link, no EMEVD. `msbe_parser` reads Enemy(2)/Asset(13) only; adding
   type 5 would give the cleanest source **for seamless dungeons**. Likely MIXED: ConnectCollision for
   seamless, `WarpPlayer` for loading-warp catacombs — area 45's type (seamless vs warp) is unknown.

Next: (a) extend `vmap emevd` to all-instruction dump to catch bank-2003 `WarpPlayer`, and/or (b) add a
ConnectCollision probe to the MSB parser. Validate either against a30's `(10913,8522)`.

## EMEVD warp decode — round 2 (all-instruction dump): (a) RULED OUT for catacombs

Extended `vmap emevd <map> banks` (bank:id histogram over ALL instructions) + `bank <N>` (dump every
instruction of a bank with args). Dumped **every** instruction of catacomb **m30_04_00_00** (145 instrs,
38 distinct bank:id):

- All warp-plausible banks reference only the catacomb's OWN entities/flags — **zero overworld / cross-map
  reference, no destination-map arg, no overworld coords:**
  - `bank 2003` (flags/events): `id=43 args=[3,9111,0,1]` (flag-range), `id=66` SetEventFlag(`30040800`),
    `id=11/12/69` — all own-map.
  - `bank 2004` (character ctrl): `id=1/4/5/34/39 args=[30045800,…]` — enable/disable the catacomb's own
    characters.
  - `bank 2009 id=3 args=[300400,30041950,0,0,0,5.0f]` — `300400` = m30_04_00 packed map-id = a SELF
    reference (spawn), not a warp out.
- `vmap emevd m30_04_00_00 60` → 0 (no init references area 60 / overworld).

**Conclusion: catacomb (loading-warp) transitions are NOT encoded as EMEVD warps.** The interior EMEVD
has no knowledge of its overworld entrance. Path (a) — EMEVD `WarpPlayer` decode — is a **dead end for
warp-dungeons** (it may still exist for other transition types, but not the dungeon entrance we need).

## → Pivot to (b): MSB `ConnectCollision` / warp-point asset

The overworld↔dungeon link is engine-handled at the MSB level. Candidates to probe next:
1. **`ConnectCollision` (MSB part type 5)** — carries the target `MapID[4]` (area/block/cc/dd) + a
   position. If the OVERWORLD MSB has a ConnectCollision with `MapID` area=45, its position is the
   entrance. `msbe_parser` reads Enemy(2)/Asset(13) only — add type 5.
2. **Entrance ASSET + its region/ObjAct** — the cave-mouth asset; needs a map link (may reference the
   dungeon via a param or the ConnectCollision above).
Next probe: parse the overworld MSB's ConnectCollision parts (dump `MapID` + pos), validate a known
catacomb's ConnectCollision position ≈ its `fold_probe` entrance, then use it to seed `entrance_anchor`.
Coverage caveat: ConnectCollision may only exist for SEAMLESS dungeons; if catacombs lack it too, the
entrance link may live only in the asset/region layer (deeper MSB RE).

## Path (b) round 1 — ConnectCollision EXISTS in catacombs (new `vmap msbparts` probe)

New probe `vmap msbparts <mapName> [partType]` dumps MSB Parts (type @+0x0c, name, pos @+0x20, first 8
typeData words @+0x68). Part-type histogram of **m30_04_00_00** (541 parts, 8 types): type 0 MapPiece×2,
2 Enemy×24, 4 Player×3, **5 Collision×7**, 9 DummyEnemy×2, 10 DummyAsset×4, **11 ConnectCollision×4**,
13 Asset×495. So **catacombs DO have ConnectCollision (type 11)** — path (b) is viable (contrast: EMEVD
had nothing).

The 4 ConnectCollisions: names `h000100_0000`..`h000400_0000`, **pos `(0,0,0)`** (they reference a
Collision by index, not a position), `td[0]` = 0/1/2/3 = the **CollisionIndex**. `td[1..]` at the +0x68
sub-struct are NOT the MapID (values like `0xFF2630BC` = wrong block — +0x68 is the entity/common slot,
not the ConnectCollision type struct).

**Remaining decode (next step):** (1) find the ConnectCollision **type-struct** offset in the part header
(the ER MSBE ConnectCollision struct is `int CollisionIndex; sbyte[4] MapID; …` — read the correct
sub-offset, not +0x68) → the target map. (2) The entrance POSITION is not on the ConnectCollision itself
(pos=0) — it's on the referenced **Collision (type 5)** part at `CollisionIndex`, OR (cleaner) it's the
OVERWORLD tile's ConnectCollision→m45 whose referenced overworld Collision gives the world position. So:
dump the OVERWORLD tile's type-11 parts, find `MapID==m45`, follow its CollisionIndex to the type-5
Collision's position = the entrance. Validate a known catacomb's chain lands at its `fold_probe` entrance.
The probe (`vmap msbparts`) is in place; the layout offsets are the only unknown left.
