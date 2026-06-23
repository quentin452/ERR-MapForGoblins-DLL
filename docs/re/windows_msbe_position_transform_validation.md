# Findings — MSB position → bake transform link VALIDATED (the risky wiring link)

Validated the riskiest migration link: does the runtime MSB Part position, transformed, land on the
bake's markers? Method: parsed **all of ERR's real `.msb.dcx`** (offline, Python mirror of the C++ parser)
and diffed Treasure positions against `data/items_database.json`. 2026-06-24. Scripts
`D:\ghidra_scripts\diffall.txt` / `diff00.txt` (probes `/tmp/diffall.py`, `/tmp/diff00.py`).

## ★ Result — the bake stores RAW MSB block-local; no transform is baked into x/y/z
Confirmed on 3 map types: `m60_37_50_00` chest `(56.33,237.93,52.68)`, `m10_00` `(-298.40,64.31,426.31)`,
`m60_42_36` `(-53.30,92.91,91.32)` — all = the raw MSB **Part position @+0x20**, with `gridX/gridZ/areaNo`
stored SEPARATELY (from the map name / the part's `gridXNo` field). `extract_markers.py` applies the
world transform DOWNSTREAM: `world = gridXNo·256 + pos` (+ `WorldMapLegacyConvParam` base-point for legacy
dungeons, lines 295–303). So the runtime parser only needs to reproduce `(grid, localPos)` — **the existing
transform is reused as-is, no new RE.**

## ★ Diff (all 708 DFLT maps, then LOD0-only)
| scope | MATCH | MISMATCH | msb-lot-not-in-bake | noPart(0xffff) |
|---|---|---|---|---|
| all DFLT (708) | 3628 | 38 | 393 | 16 |
| **LOD0 `_00` only (651)** | **3374** | **24** | 373 | 14 |

**99.3% exact position match (≤0.5u total).** The remaining 24 are NOT parser errors:
- **lot shared across parts / same part in 2 maps** (e.g. `m31_90` & `m33_00` both carry lot 11100000) —
  when the bake part-name matches the MSB part, positions agree to ~2.5u (marker-exact); when they differ,
  the lot legitimately has multiple placements and the diff just picked a different valid one.
- The all-vs-LOD0 drop (38→removing the m60 ones) = **low-LOD connect tiles `m60_XX_YY_02`** whose parts
  are proxies (name prefix `m60_AA_BB_00-`) re-referencing detail-tile parts at a 128/256-offset → the real
  placement lives in the `_00` detail tile.

## Wiring rules (for the C++ parser → marker feed)
1. **Parse LOD0 maps only** (`m*_*_*_00.msb.dcx`); skip `_01/_02/_99` (connect/LOD/lighting variants) —
   they re-reference detail parts and would double/misplace markers.
2. **Position = Part+0x20 = the bake's block-local `x/y/z`** — verified across overworld detail + all legacy
   dungeons. Feed it through the SAME `extract_markers` transform (`gridXNo·256 + pos` + legacy-conv).
3. **Grid/area**: derive from the map filename (`m60_37_50_00` → area 60, gridX 37, gridZ 50); for overworld
   detail tiles this equals the part's `gridXNo`/`gridZNo` field. Legacy dungeons: map id + legacy-conv.
4. **A lotId can map to MULTIPLE parts** → emit a marker for each (don't dedup to one). The 24 "mismatches"
   were exactly this (the diff collapsing multi-placement lots).
5. `partIndex == 0xFFFFFFFF` (≈14–16 treasures) = item-glow / no physical part → no MSB position (handle via
   region/EMEVD, or leave to the separate EMEVD-position workstream, commit 607cb65).

## Note — 373 MSB treasure lots NOT in the bake's treasure set
Potential runtime ADVANTAGE (ERR-added loot the MSB-bake slice missed) OR lots the bake filed under
`enemy`/`emevd`. Worth a quick characterization during wiring (are they real placed treasure absent from
the shipped DB?). Not a blocker for the transform link.

## Verdict
The transform link is **de-risked**: 99.3% exact, residual = multi-placement matching artifacts (not parser
bugs), and the world transform is **reuse-existing** (no new RE). The MSB position pipeline is ready to wire
behind the LOD0 + multi-part rules above. Probes: `/tmp/diffall.py`, `/tmp/diff00.py`;
out `D:\ghidra_scripts\diff{all,00}.txt`.
