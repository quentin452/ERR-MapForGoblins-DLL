# MSBE DummyAsset / unreachable-loot filter — RE findings (2026-06-24)

Context: the disk-MSB loot source (config `loot_from_disk_msb`, see `msbe_parser` +
`worldmap/loot_disk`) emitted ~178 markers that the committed bake does NOT show. This
documents why, the one-field fix, and — importantly — the small set of lots the fix DROPS
that are still bake-backed today (must be recovered before the bake can be removed).

## Why the disk had 178 "extra" markers
The offline pipeline (`tools/extract_all_items.py:390-516`) excludes **312 "unreachable_only_lots"**
(`data/unreachable_msb_lots.json`) via FOUR criteria:
1. **Asset `GameEditionDisable=1`** — placement disabled in this build (debug/removed).
2. **Overridden treasure** — same MSB part referenced by multiple Treasure events with different
   lots; only the LAST wins at map load, earlier lots never deliver. (MSB-derivable, offset-free.)
3. **Dummy-only, no Entity** — the Treasure binds only to a `Parts.DummyAssets` part whose
   `EntityID` AND `EntityGroupIDs` are all zero → no EMEVD can activate it → unreachable.
4. **Position-clip vs vanilla** (`tools/unreachable.py`, 13 manual entries) — ERR moved the asset
   under terrain / out of reach; needs the VANILLA map Y to detect → **NOT runtime-derivable.**

The raw disk walk has none of these filters → it re-emitted all 312.

## RE WIN — part-type at PART entry +0x0c (validated over all 487 ERR DFLT maps)
MSBE `PartsParam` entry, byte **`+0x0c` = part-type enum**:
- **`13` = Asset** (real, reachable placement)
- **`9`  = DummyAsset** (disabled / cut placeholder)

Validated empirically: in m20, every *reachable* lot is type 13; 24/25 *unreachable* are type 9.
Across all maps: **305 of the 312 unreachable lots are type 9.** So:

> `msbe_parser` now reads `Treasure.partType` (`pe+0x0c`); `loot_disk` DROPS DummyAsset (type 9)
> placements. One field read explains **97.8%** of the pipeline's exclusions — no EntityID offset,
> no baked exclusion list. In-game: disk-only markers collapsed **178 → 21**.

### ✅ EntityID / EntityGroupIDs offset — PINNED (2026-06-24)
The part's **entity sub-struct pointer is at PART entry `+0x60`** (an entry-relative u64 on disk /
absolute VA resident — same base rule as nameOffset/typeData). Inside that sub-struct:
- **`+0x00` = `EntityID`** (u32)
- **`+0x1c` .. `+0x38` = `EntityGroupIDs[8]`** (8× u32; unused slots = 0)
- (`+0x0c` = 0x01000000 flags, `+0x10` = 0x100, `+0x3c` = 0xffff — unrelated)

Pinned via SoulsFormats (authoritative `EntityID`/`EntityGroupIDs`) cross-referenced against the raw
blob over the 3 reachable_dummy targets + enemy parts with non-zero groups (`tools/probe_entity_offset.py`,
`tools/probe_group_offset.py`). The other part-header u64s seen in the same entry (entry-relative):
`+0x18→sib path (0xe0)`, `+0x50→0xe8`, `+0x68→0x2b0` (display groups), `+0x70→0x2d0`, `+0x88→typeData`.

**RULE NOW IMPLEMENTED (`msbe_parser` reads `Treasure.entityId`/`entityGroup`; `loot_disk` KEEPS a
type-9 part iff `entityId != 0 || entityGroup`).** Validation over ALL ERR `_00` maps
(`tools/validate_dummy_entity.py`): exactly **3** type-9 lots are reachable (entity set), **0** of
them are in `unreachable_msb_lots.json` → the keep-rule re-introduces **zero** false positives (the
178→21 win holds; 305/315 inert dummies still correctly dropped). Targets **4910 (EntityID 12031490)
+ 15000990 (EntityID 15001810) are recovered** — now disk-emitted, no longer bake-dependent.

## ⚠️ RECOVER-LATER — reachable_dummy lots the filter DROPS but the bake still backs
A lot whose ONLY MSB part is a DummyAsset (type-9-only) but that the pipeline KEEPS (not in
`unreachable_msb_lots.json`) is a **reachable_dummy** — the DummyAsset has an EntityID/group, so an
EMEVD activates it and it's a real pickup. Our `type==9 → drop` rule drops it from the disk source,
but it renders fine TODAY via its baked marker (coverage-replace keeps uncovered baked rows → **zero
loss now**). It would be **LOST the day the committed bake is removed**.

Exact set (authoritative = runtime `baked_lot1 ∧ ¬disk_lots`, i.e. bake shows it AND no disk Asset
twin emits it; **3 lots**, confirmed in-game 2026-06-24):

| lotId | tile | item key | note |
|---|---|---|---|
| ~~`4910`~~ | m12 (Siofra/underground) | 500008183 | ✅ RECOVERED — DummyAsset EntityID 12031490, now disk-emitted |
| ~~`15000990`~~ | m15 (Haligtree/Elphael) | 500002190 | ✅ RECOVERED — DummyAsset EntityID 15001810, now disk-emitted |
| `2046460000` | m61_46_46 (DLC overworld) | 502010000 | ⚠️ NOT recoverable via this route — its DummyAsset has **EntityID 0 + all groups 0** (no MSB entity binding) AND is itself in `unreachable_msb_lots.json`. Entity-less (EMEVD lotId-direct / cut). Stays bake-backed; the lone residual recover-later lot. |

**Net: recover-later 3 → 1.** The two entity-bound dummies (4910/15000990) are now emitted from the
disk source like any Asset — identical marker/position as today (disk pos = bake pos), but they
survive when the committed bake is removed. 2046460000 is the only DummyAsset the bake shows that the
disk can't reconstruct (no entity); recovering it needs the EMEVD route, not the part offset.

(An earlier offline proxy "type-9-only ∧ not-in-unreachable" listed 4910/90200/15000990, but 90200 is
NOT in the bake's lotType==1 set so it's not actually shown today; the runtime oracle is correct.)

Runtime trace: `[LOOTDISK] RECOVER-LATER reachable_dummy lot <id> key=<k>` is logged for every dropped
DummyAsset whose lotId IS in the bake's lotType==1 set AND is NOT disk-covered (no Asset twin) — the
exact set lost the day the bake is removed. Lots with a live Asset twin are excluded (disk still emits
them).
**To remove the bake fully:** pin the EntityID/group offset (above) so the disk path KEEPS type-9
parts with a non-zero EntityID/group instead of dropping them — that recovers these without the bake.

## False positives still shown (the 21 disk-only residual after the DummyAsset filter)
Benign over-display (0.6% of 3457; no MISSING loot). Breakdown:
- **Position-clip (~7, in `unreachable_msb_lots.json`)** — e.g. Golden Seed twins under the Capital
  wall `m60_42_51` / `m60_43_52` (goods 10010), Altus crafting node `m60_49_40`, plus m39/m31/m20_00.
  Need vanilla geometry → bake a tiny list or accept.
- **type-10 crafting subtype (~10)** — `m21_02` `AEG463_610_90xx` block, all goods 2020001 (crafting
  material). Part-type **10** (a third subtype, ≠ Asset/DummyAsset); the bake doesn't emit them as
  lotType==1 markers. Filtering type 10 would remove these (offset-free, same `pe+0x0c`).
- **extracted-but-unlinked (~4)** — in `data/items_database.json` but not `loot_lot_linkage.json`
  (the bake extracted then deliberately didn't mark them), e.g. m10/m60 overworld lots.

## Files
`src/worldmap/msbe_parser.{hpp,cpp}` (partType @ +0x0c), `src/worldmap/loot_disk.{hpp,cpp}` (drop
DummyAsset + report dropped lots), `src/worldmap/map_entry_layer.cpp` (recover-later + disk-only
diagnostics). Commits 6f62ed7 (wiring), b8b2911 (DummyAsset filter). Offline probes were ad-hoc
(replicate the `parse_msb` section walk in Python over the mod's `map/MapStudio/*.msb.dcx`).
