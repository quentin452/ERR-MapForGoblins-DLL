---
name: extra-graces-siofra
description: "ROOT-CAUSED 2026-06-23 (NON-PRIORITY, parked): overlay draws phantom graces (Siofra + Lake of Rot 'Underground's End') = BonfireWarpParam rows for unplaced/cut bonfires our gate can't filter. Two classes + fix plan recorded."
metadata:
  node_type: memory
  type: project
---

BUG (user 2026-06-23, in-game overlay map): overlay shows graces that vanilla ER / MapGenie do
NOT have. Reported spots: **Siofra River** (nameless extras) + **Lake of Rot / Astel Naturalborn**
("Underground's End" grace, next to the real "Beneath the False Stars"). **ROOT-CAUSED. Marked
NON-PRIORITY by user — parked, NOT fixed.** Diagnostic + fix NOT shipped (all reverted, master clean).

## Root cause (verified via a temporary [GRACE-DUMP] of all 438 BonfireWarpParam rows, now reverted)
Graces are read LIVE from BonfireWarpParam (`capture_live_graces`, goblin_inject.cpp ~910). Gate =
`areaNo!=0` + `eventflagId>0` + `textId1>0` + `(dispMask0 & 0x7)!=0`. Two phantom CLASSES slip through:

**CLASS 1 — NAMELESS (the Siofra extras).** BonfireWarpParam rows whose `textId1` has **NO PlaceName
entry** → render as a nameless/null icon. Our gate only checks `textId1>0`, NOT that the name
resolves. The OLD offline baker (deleted `tools/generate_graces.py`) explicitly skipped these
("textId1 has no PlaceName text → would be a nameless/null icon"). **FIX (clean, general):** in the
capture, skip if `goblin::lookup_text(textId1)==nullptr` (goblin_messages.hpp — returns null for
missing ids). ⚠️ ORDER CAVEAT: `setup_messages` (builds the PlaceName FMG) runs AFTER
`capture_live_graces` in dllmain (init step line 201 vs 194) → gate must move to grace-marker-BUILD
time (grace_layer) or reorder init, else lookup_text isn't ready at capture.

**✅ CUT-GRACE CONFIRMED (user in-game 2026-06-23).** After the grace-anchor live migration (commit
4a8022a) the ~3 off-map base-underground phantom graces PERSIST unchanged → not a projection/anchor
artifact (that migration ruled it out). They are genuine BonfireWarpParam rows for unplaced/cut
bonfires, exactly as root-caused. Confirms the fix must be a grace-FILTER (class 1/2 below), not a
projection fix.

**CLASS 2 — NAMED CUT GRACE ("Underground's End").** Decisive dump rows (area 12 = base underground,
grid (4,0), Lake of Rot subCat 12011):
```
row=120400 entity=12041950 flag=71240 dispMask0=0x02 icon=1 pos=(-64.5, -77.8) textId1=120400  "Beneath False Stars"  REAL
row=120401 entity=12041951 flag=71241 dispMask0=0x02 icon=1 pos=(-66.6,-307.3) textId1=120401  "Underground's End"    PHANTOM
```
Param fields are **IDENTICAL** to a real grace (valid PlaceName 120401, dispMask set, flag>0, entity
just real+1) → **indistinguishable from BonfireWarpParam alone.** It's a real ER param row for a
bonfire that's never PLACED/REGISTERED in the world (vanilla + MapGenie don't show it). NOT a DLC-leak,
NOT a projection bug (both rows are genuine area-12 base-UG). **Would have appeared in the OLD bake
too → pre-existing latent issue, NOT a regression** from the live-grace switch (commit cbbec2e). The
old baker's two extra filters do NOT catch it: `is_unreachable_grace` keys on MSB Y-displacement (no
m12 entry) and `EXISTING_TILES` is tile-level (m12_04 exists for the real grace). Real discriminator =
**the bonfire entity has no MSB asset.**

## Fix plan (parked)
- Class 1: `lookup_text != null` gate (see order caveat). General, no baked data.
- Class 2: needs the set of bonfire entityIds actually placed as MSB assets → allowlist/blocklist.
  - PROPER general fix = offline extract bonfire-asset entityIds from the MSBs → baked allowlist, DLL
    filters live graces whose entity ∉ allowlist. **BLOCKED on Linux: MSB `.dcx` = Oodle Kraken**
    (oo2core won't load natively, same wall as [[loot-identity-stable-err-additive]] / baked-loot-pos).
    Do the extract on a Windows box (user's Windows RE agent).
  - INTERIM = curated DLL entityId **blocklist** (12041951 + any others found by auditing the live
    grace list vs MapGenie). Matches the project's existing curated-exclude pattern (`tools/unreachable.py`
    `is_unreachable_grace`). Whack-a-mole; needs a manual grace audit to be complete.

Diagnostic used: `[GRACE-DUMP]` (per-row dump of BonfireWarpParam in capture_live_graces) — built,
run 3× (underground-only → all rows → +entity/dispMask1), conclusions above, then **fully reverted
(master working tree clean, clean DLL redeployed)**. Relates to [[overlay-rendered-markers]] grace work,
[[err-underground-grace-gate]] (iconId==44 gate — NOT the cause here; base-UG area-12 graces use icon=1),
[[live-param-vs-baked-data]] (MSB allowlist = hybrid-acceptable baked data).
