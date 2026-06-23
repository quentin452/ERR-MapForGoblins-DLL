# RE prompt — live loot/treasure WORLD position (the last baked loot field)

> **Goal: complete the loot runtime migration.** Loot marker IDENTITY is now read LIVE from
> ItemLotParam (shipped: `resolve_loot_item_textid`, baked textId1 stripped from lot-backed rows).
> POSITION is the only loot field still baked (from offline MSB extraction). This RE finds the
> live treasure/asset world position so position can go runtime too — the cleanest-by-design end
> state (zero baked loot data). App = current ERR build; re-anchor every RVA.
>
> **⚠️ Honesty up front — this is design-purity, NOT a bugfix.** Position does NOT drift: ERR is
> ADDITIVE (adds items) and a randomizer shuffles lot *contents*, never moving the chest/enemy. So
> the success criterion is "**live position == the baked position**" (validates the read), not
> "fixes wrong markers". And it is HIGH effort: unlike identity (a param, no RE), position lives in
> the runtime MSB/asset placement system → real memory RE. Weigh that before committing fleet time.

---

## 0. Cheap pre-check FIRST (may make this RE unnecessary)

The only thing that actually breaks baked positions is ERR **relocating/adding** chests in a newer
version than the MSB we extracted `items_database.json` from. Check that offline, no RE:
- Re-extract from the CURRENT ERR regulation+MSB and diff `items_database.json` positions for ~all
  lot ids. If positions match the committed bake → **skip this RE**; the bake is correct, just
  re-extract on each ERR version bump (a coverage/position diff is the real future-proof, cheaper
  than a runtime read). If many differ → ERR moves content → a runtime live-position read earns its
  keep → proceed below.
- If the goal is purely runtime-purity (read everything live regardless of drift), skip the
  pre-check and proceed — but know it buys cleanliness, not correctness.

Context on what's already live (so you reuse, not rebuild): loot identity = `ItemLotParam` slot-1
(`resolve_loot_item_textid`, goblin_inject.cpp), pickup flag = `resolve_loot_flag` (same LotReader),
graces = live BonfireWarpParam, bosses = live WorldMapPointParam 5100. Player pos + the block→world
/ map-UI transforms are RE'd (re_findings_playerpos.md). Position is the one piece with no param
source.

---

## 1. Target

Read each loaded treasure/drop's **live world position + its ItemLotID** at runtime, so loot
markers can be placed from the live asset placement instead of the baked MSB coords (analogous
to how bosses now read live WorldMapPointParam rows).

### T1 — the treasure/asset placement system
ER places treasure via MSB `Events.Treasure` linking a chest **asset** (MSB `Parts.Asset`) to an
`ItemLotID`. At runtime the asset is a loaded instance with a world transform. Find:
- the manager that owns loaded **asset instances** (likely the same world-geom system we already
  touch for collected state — `WORLD_GEOM_MAN_SLOT` in `src/re_signatures.hpp`, used by
  goblin_collected). Confirm whether asset instances carry both a **world position** and a link
  to their **ItemLotID** / MSB entity id.
- per asset instance: world pos (X/Y/Z), MapId (for the area/grid + the block→world transform we
  already own — `FUN_1408775e0`/`FUN_140876140`, see re_findings_playerpos.md), and the
  ItemLotID (or MSB entity id we can map to a lot).

### T2 — enemy-drop positions
For enemy-drop lots (`lotType==2`), the position is the enemy ChrIns world pos. This overlaps the
abandoned boss/enemy WorldChrMan enumeration (windows_enemy_boss_runtime_pos_re_findings.md) —
note that unloaded enemies have no instance, so this only works in loaded regions.

### T3 — join key to our markers
Our markers carry `lotId` (ItemLotParam row id) + `lotType` (1=map,2=enemy) via
`loot_lot_linkage.json`. The live asset must expose its ItemLotID (or an MSB entity id we can map
to the lot) so a live position can be matched back to the right marker.

---

## 2. Deliverables (`windows_live_loot_position_re_findings.md`)
1. **§0 pre-check verdict first**: do baked positions actually drift vs live ERR? If no → stop.
2. The asset/treasure manager: static base + per-instance offsets for world pos + ItemLotID +
   MapId, each with AOB/RVA + a live sample.
3. Whether it reuses the existing `WORLD_GEOM_MAN_SLOT` geom manager (preferred — we already walk
   it) or a separate treasure/ItemDrop system.
4. The unloaded-region limitation (only loaded assets have a live pos) — scope it.

## 3. Notes
- Reuse what we own: the block→world transform (`FUN_1408775e0`) + `(MapId,local)`→map-UI
  (`FUN_140876140`), WorldChrMan static, the geom manager (`WORLD_GEOM_MAN_SLOT`).
- This is HIGH-EFFORT (live MSB/asset placement RE) for a drift that is usually zero. Loot
  identity is ALREADY live (shipped, no RE — ItemLotParam param read); position is the only loot
  field left baked, so this RE is the optional last step to a fully-runtime loot marker.
- Read params/structs LIVE (ERR row values differ from vanilla); never assume vanilla data.
