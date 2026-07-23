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
