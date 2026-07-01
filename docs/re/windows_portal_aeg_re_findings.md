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

## SOLVED — EMEVD template `90005605` = the sending-gate warp. CONFIRMED.

`tools/_probe_portal_emevd.py` + `_probe_portal_verify.py`: scanning bank-2000 `InitializeEvent` calls
(arg[1]@off4 = called template, args off8+ = params) for the AEG099_510 entity set finds **template
`90005605`** — **23 distinct AEG099_510 gates at arg[2] (offset 8), 27 calls** — in the shared
`90005xxx` common-template range (same family as seal `90006051` / hero-tomb `90005683`). The other
match, `1045630910` (50 gates at arg[7]), is a Leyndell-LOCAL id = the decorative 94-cluster's warp, NOT
the generic template.

Verified (`_probe_portal_verify.py`): the 25 distinct arg[2] entities (23 are AEG099_510; 2 use a
different model/region) span the whole world — m11 Leyndell, m12 Siofra, m14 tunnels, m34, DLC m61, and
Limgrave/Caelid overworld tiles (m60_33..51). The call args are a textbook warp: `arg2 = gate entity`,
`arg4 = destination entity`, `arg5 = warp target`, `arg6..8 = sub-entities`. LOD duplicate tiles
(`_00` + `_10`) repeat the same entity → dedup by entity ⇒ **23 unique sending gates.**

MapGenie's "Portal" (39) is broader (also belfry imbued-key portals / DLC-entry, likely other
mechanisms), but `90005605` gives a clean, well-defined **sending-gate** set — the correct core.

## Implementation (clean, mod-agnostic — mirrors seal_emevd/hostile-NPC)

**Portal = an AEG099_510 asset whose EntityID is bound as arg[2] of an EMEVD `90005605` call.** Runtime,
no bake: (1) harvest the sending-gate entity set live from the active install's `event/*.emevd` (template
90005605, entity@arg[2]) — same EMEVD-template harvest the world-feature flag passes already do; (2) a
disk asset pass emits each AEG099_510 placement whose EntityID ∈ that set (dedup by entity across LOD
tiles); (3) new `WorldPortal` category + plumbing. Portals never "complete" → no graying flag
(`flag_rule` none). Model + template facts are mod-agnostic (base-game AEG + common template, present on
every install).
