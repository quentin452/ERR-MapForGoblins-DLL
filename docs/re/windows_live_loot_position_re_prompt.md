# RE prompt — live loot/treasure WORLD position (CONDITIONAL — read §0 first)

> **⚠️ This RE is probably NOT what the loot drift needs. Read §0 before starting it.**
> The dominant loot drift is IDENTITY (what item), which is fixable with NO RE. Live POSITION
> only matters if ERR physically relocates/adds chests vs the MSB we extracted — verify that
> first. App = current ERR build; re-anchor every RVA.

---

## 0. Decide FIRST: do we even need this? (the drift is identity, not position)

Lot-backed loot markers carry two baked things (see the loot data-flow audit):
- **Position** `posX/posZ/areaNo/grid` — baked from MSB `Events.Treasure` (chest asset) /
  `Parts.Enemies` (drop) placement (`tools/extract_all_items.py` → `items_database.json`).
- **Identity** `textId1` (item name id, category-offset-encoded) — baked from `ItemLotParam`
  item1..item8 at extraction time.

Under a randomizer-adjacent mod (ERR), the **contents** of a lot change but the **chest/enemy
stays put**. So:
- **IDENTITY drifts** (baked textId1 ≠ the live randomized item) — this is the real, large drift.
- **POSITION is stable** (the chest doesn't move) — so live position is usually unnecessary.

**The identity fix needs NO reverse engineering** — the live source is `ItemLotParam`, a param:
`resolve_loot_flag()` (goblin_inject.cpp) already reads the live `ItemLotParam` row via the
`LotReader`/`from::params` path, and already reads `row->b + 0x04` (item2). item1 is at
`row->b + 0x00`; its category gives the FMG offset (Goods 500M, Weapon 100M, …). The
`liveLootLabels` FMG preload already copies every item name into PlaceName at its offset-encoded
id. The only missing piece is the aspirational `refresh_loot_from_itemlot()` — pure code, not RE.
**Do that before any of the below.**

**So only proceed with this RE if a pre-check shows ERR moves/adds chests vs our extraction:**
- Pre-check (cheap, no RE): pick ~20 lot-backed markers, compare our baked `(areaNo,grid,pos)`
  to the live ERR MSB/asset placement for the same `itemLotId` (offline, re-extract from the
  current ERR regulation+MSB and diff `items_database.json`). If positions match → the bake is
  fine, **skip this RE entirely** (just re-extract on each ERR version bump). If many differ →
  ERR relocates content and a runtime live-position read is worth it → continue.

---

## 1. Target (only if §0 says positions actually drift)

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
- This is HIGH-EFFORT (live MSB/asset placement RE) for a drift that is usually zero. The
  identity-live fix (§0, no RE) is the high-value, low-cost win — prefer it.
- Read params/structs LIVE (ERR row values differ from vanilla); never assume vanilla data.
