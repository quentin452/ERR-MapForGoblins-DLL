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

The other 3 part-section offsets seen in a part entry (entry-relative): `+0x18→0xe0`, `+0x50→0xe8`,
`+0x60→0x270`, `+0x68→0x2b0`, `+0x70→0x2d0`. The `EntityID`/`EntityGroupIDs` live inside one of
these sub-structs (not inlined; +0xc0..+0xd4 are shared draw/display-group masks) — **offset not yet
pinned** (a reachable_dummy vs inert compare in m14 didn't surface a clean field). Pinning it is the
work needed for criterion #3 at runtime if we ever want zero bake dependency.

## ⚠️ RECOVER-LATER — reachable_dummy lots the filter DROPS but the bake still backs
A lot whose ONLY MSB part is a DummyAsset (type-9-only) but that the pipeline KEEPS (not in
`unreachable_msb_lots.json`) is a **reachable_dummy** — the DummyAsset has an EntityID/group, so an
EMEVD activates it and it's a real pickup. Our `type==9 → drop` rule drops it from the disk source,
but it renders fine TODAY via its baked marker (coverage-replace keeps uncovered baked rows → **zero
loss now**). It would be **LOST the day the committed bake is removed**.

Exact set (offline ground truth = type-9-only ∧ not in unreachable; 3 lots):

| lotId | tile | note |
|---|---|---|
| `4910` | m12 (Siofra/underground) | recover via Entity/group |
| `90200` | m60_43_37 (overworld) | recover via Entity/group |
| `15000990` | m15 (Haligtree/Elphael) | recover via Entity/group |

Runtime trace: `[LOOTDISK] RECOVER-LATER reachable_dummy lot <id> key=<k>` is logged for every dropped
DummyAsset whose lotId is still in the bake's lotType==1 set (the runtime oracle for "bake-backed").
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
