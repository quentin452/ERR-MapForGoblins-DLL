# Findings — Portal / waygate (MapGenie Group 2) AEG source

Goal: wire MapGenie's **Portal (39)** category. RE findings (`windows_mapgenie_category_coverage_re_findings.md`
Tier 2B) said portals are NOT a clean `WorldMapPointParam.iconId` (iconId 87 is impure — mixes Sending
Gate with Volcano-Manor-request markers) and are "MSB warp assets." This pass identifies the model but
shows it is **not** a clean one-row `world_feature_assets.py` add.

Tools: `tools/_probe_portal_aeg.py` (model identification via WMPP anchor), `_probe_portal_aeg2.py`
(entity characterisation). Both read the active install's `map/MapStudio/*.msb.dcx` via SoulsFormats.

## Model IDENTIFIED — `AEG099_510` = the sending-gate asset. CONFIRMED.

The single WMPP row named "Sending Gate" (m60_46_39, pos −59.2,−124.5) has exactly one AEG asset on it:
**`AEG099_510` at distance 0.4u** — an unambiguous anchor hit. So `AEG099_510` is the sending-gate model.

## But it is NOT a clean portal set — 180 placements, mostly non-portal. HYPOTHESIS (one-row add) WRONG.

`AEG099_510` has **180 placements, all with EntityID>0**, spread across areas
{10,11,12,13,14,15,16,20,21,28,31,34,35,60,61}. Entity-id families (`_probe_portal_aeg2.py`):

- **13 map-local-prefixed** ids (`11001692`, `12021670`, `14001660`, … — id starts with the map's
  `AABB`): the map-scripted interactive gates. Areas {10,11,12,14,16,34} only (legacy dungeons /
  underground). These look like the real scripted sending gates.
- **148 in a shared `10456xxxx` range** — the bulk, and NOT one-portal-each. **94 of them share entity
  group `1045635900`** (a tight decorative cluster in Leyndell m11_10, positions all within a few metres)
  — clearly visual/anchor geometry, not 94 portals.
- **19 other.**

So raw "emit every `AEG099_510`" (or "every entity-bearing one") would draw ~180 markers vs MapGenie's 39,
including a 94-blob in Leyndell — wrong. `entity_required=True` does NOT filter here (all 180 have an
EntityID), unlike the seal/statue features.

## What's needed to isolate the ~39 real portals (NEXT RE step, not done)

The player-facing signal for "usable sending gate" is almost certainly the **EMEVD warp event** that binds
the gate entity to a destination (the sending-gate activation/warp template). Correlating `AEG099_510`
placements to that template's entity arg — the same shape as the existing `seal_emevd` / `hero_tomb_emevd`
flag joins (`msbe_parser` `kEmevdFlagTemplates`) — should yield the interactive subset. That template id +
its entity-arg offset are the remaining unknowns (an EMEVD scan, `tools/datamine_emevd_*.py` style).

Alternatively: the 13 map-local-prefixed ids may already be a usable (if partial) portal set for the
dungeon/underground gates; the overworld/DLC waygates would still need the EMEVD join.

## Recommendation

Portal needs the EMEVD warp-template RE before it can be wired cleanly (it is NOT a `world_feature_assets`
one-liner). Options: (a) do that EMEVD scan next; (b) approximate with the 13 local-prefixed gates + a
dedup'd, cluster-collapsed subset of the shared range (imperfect); (c) pivot to an easier Group-2 category
first (Smithing Table = a single AEG model; Elevator = AEG lift models) and return to Portal with the
EMEVD tooling. The model fact (`AEG099_510`) is solid and reusable whichever path.
