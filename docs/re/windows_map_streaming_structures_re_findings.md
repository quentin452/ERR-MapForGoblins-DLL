# Findings — ER map-streaming structures (do they give item positions / lotIds?)

Hypothesis (quentin): ER streams map files from disk during play; maybe the loaded map data exposes item
positions (and lotIds) pre-spawn. Investigated live (game running) with the reusable tools
(`tools/ghidra/rtti_index.txt` + `query.java`) + a vtable-instance scan (`msb_event_scan.py`), 2026-06-24.

## What's resident (the streaming IS real — intuition confirmed)
Vtable-scan counts on the running game:
| structure | vtable RVA | live count | holds |
|---|---|---|---|
| `CSMapbndResCap` | `0x2ba5658` | **267** | the `.mapbnd` bundles streamed from disk; raw bundle is a ref-counted resource at **+0xC0** (`this[0x18]`, released in dtor `FUN_140ce9ee0`) |
| `CSMsbPartsGeom` | `0x2ba6738` | **8001** | parsed placed geom parts (chests/assets) **with transforms (positions)** |
| `CSMsbPartsMap` | `0x2ba68f0` | **343** | parsed map parts (= the 343 `CS::MapIns`) |
| `CSMsbParts` (base) | `0x2ba6418` | 1 | — |
| `CSMsbEvent` | `0x2ba6b40` | **1** | the MSB **events are NOT kept** as resident instances |

So ER keeps the full parsed PART set resident (8001 geom + 343 map) while blocks are loaded → **all
placed-part positions are resident**. (The mod already taps a subset of this via the `CSWorldGeomMan`
geom walk.) `CSMapbndResCap`=267 confirms continuous disk streaming of map bundles.

## The catch — Treasure events (itemLotId) are consumed at parse, NOT stored
- Only **1 `CSMsbEvent`** instance resident → MSB `Events` (incl. `Treasure`) are parsed at block-load
  (→ spawn the item gimmick) and **discarded**; they are not a resident, walkable structure.
- Scanning the first 0x200 of every resident `CSMsbEvent`/`CSMsbParts*`/`CSMapbndResCap` for the chest
  lotId `0x3dd6fec4` → **zero hits**. The lotId is not held by any parsed map structure.
- This is the **structural cause of §8** (`windows_fieldins_registry_layout_and_preopen_re_findings.md`):
  positions stay resident (parts), but the lotId↔part (Treasure) link does not → an UNOPENED chest has
  no resident lotId anywhere except the always-loaded `ItemLotParam` table.

## Verdict for the explore-cache
- **Positions: solved/resident** — but that was already available (geom walk). Map-streaming adds no new
  position capability, just confirms the source (8001 `CSMsbPartsGeom`).
- **Pre-open lotId via map-loading: NO** — events aren't kept. The map-loading angle does **not** unlock
  unopened-chest loot identity.
- **Only remaining memory route** to an unopened chest's lotId = parse the **raw `.mapbnd` MSB
  Treasure table** out of `CSMapbndResCap+0xC0` in memory. But that yields exactly the same data as the
  **offline bake from ERR's own map files** (which the repo already does) — redundant + high-cost (full
  MSB-format parser against a live buffer). Not worth it.
- Possible cleaner JOIN KEY (untested, likely redundant): `CSMsbPartsGeom` carries the MSB part
  **EntityID**; baking `{EntityID → lotId}` offline would let a resident-part walk join to loot by
  EntityID instead of `partName`. Marginal over the working partName join; note, don't chase.

## Net
The map-streaming structures give resident **positions** (8001 parts) but **not** resident Treasure
**lotIds** (events discarded post-parse). Confirms: keep the **baked** loot DB as the source of identity;
the runtime layer only augments for **already-spawned/opened/placed** loot (the `MapIns+0x460` walker,
`windows_mapins_to_record_reach_re_findings.md`). Sealed-chest pre-open loot stays baked-only — now
explained at the map-loading level, not just empirically.

Tooling: `D:\ghidra_scripts\msb_event_scan.py`, `tools/ghidra/{rtti_index.txt,query.java}`.
