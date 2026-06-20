# Grace placement — followups (2026-06-20)

State: the **render → screen** projection is SOLVED and pixel-exact (cyan reticle sits
on the game reticle through pan+zoom; `screen = (render − viewCentre)·zoom + canvas/2`,
scale 1.0 — commit `8bdf05c`). Graces are still misplaced because of the OTHER half of
the pipeline. Two independent error sources remain:

```
baked row ──[A: conv]──► unified world (wx,wz) ──[B: M·world+T]──► render ──[SOLVED]──► screen
            project_dungeon_row_to_overworld        g_aff (affine)
            goblin_inject.cpp:58                     goblin_overlay.cpp solve_affine
```

Fixing **B** moves every grace at once; fixing **A** corrects specific regions. Priority
order below.

---

## 1. (HIGHEST) Confirm the world→render affine `M·world + T` — measure, don't guess

The in-DLL solver is shipped (`6355a17`: `solve_affine`/`solve3x3`, hotkeys P/M/U/Del) but
the matrix is still the **seed guess** (90° axis-swap @0.5: `renderX≈0.5·worldZ`,
`renderZ≈0.5·worldX`, `T=0`). That guess is why the constellation is rotated/offset.

**Action (live, no code):** per page — solo a grace (`O`, `./,`), hover its real game
icon, press `P` (≥3 pairs/page), then `M`. Read `MapForGoblins.log` `[AFFINE]` lines:
- shared `M` should come out the SAME across pages (only `T` differs) → hypothesis holds;
- mean residual should be sub-pixel.

Pages to cover: 60, 61 (overworld), 12 (underground), 40–43 (DLC). Underground/DLC are
kept native by conv (`goblin_inject.cpp:71`) so they each need their own `T`.

**Then bake:** replace the runtime solve with the logged `M` (4 floats, once) + per-page
`T` table as constants. Anchors to sanity-check the fit:
- Dragonbarrow (page 61) world `(12338, 11985)` → render `(6018.75, 6187.28)`
- Academy (page 60) world `(9217, 13617)` → render `(1826, ?)`

**Risk / possible deeper RE:** if `M` does NOT come out shared across pages, or residuals
stay large, the transform is more than one global affine (per-region origins inside a
page, or a non-affine Scaleform step). Fallback = the external Cheat Engine route
(`docs/marker_mapspace_CT_recipe.md`) which hooks the placement thunk directly.

## 2. Transitive (multi-hop) LegacyConv — wrong region for nested dungeons

`project_dungeon_row_to_overworld` (`goblin_inject.cpp:58`) does **one** hop:
`d->areaNo = c->dst_area` once, then returns. If `dst_area` is itself a legacy sub-area
(not 60/61), the row is left in an intermediate space → wrong overworld region. Known
victims (memory): Raya Lucaria interior (area 16 → looks misplaced to Caelid/east; "no
graces in Liurnia"), Stormveil (10), Leyndell (11), Ashen Capital (35).

**Action:** loop the conv until `dst_area ∈ {60,61}` (or a native page), chaining
base-point translations: `world ← dstBase + (world − srcBase)` each hop. Guard against
cycles / missing entries (keep the current entrance-cluster fallback). Verify against a
Raya Lucaria grace landing in Liurnia, not Caelid.

## 3. Open-page filter — graces from ALL pages draw at once (overlapping)

The overlay can't read the open page: `WorldMapArea+0x6e` returned garbage (a float
bit-pattern), so the auto page-filter was disabled and the overlay draws every page's
graces, culled only by screen rect → underground/DLC/other-region icons bleed onto the
overworld. Today the user cycles pages by hand (`N`/`B`/`A`).

**RE task (read-only):** find the real open-page id. Notes from the CT recipe
(`marker_mapspace_CT_recipe.md`): the cursor's view is a *unified root* (areaNo `0`) — a
dead end. Leads: overworld-vs-underground = `DAT_143d6cfc3` (`eldenring.exe+0x3D6CFC3`);
current page = `menu+0x151` (per `marker_to_mapspace_re_findings.md`); the per-page
`CSWorldMapMenu`/`WorldMapArea` area field is still unresolved. Once known, draw only
markers whose `dstPage == openPage`.

## 4. Out-of-range rows collapse to the entrance (lose intra-dungeon spread)

The crash guard (`goblin_inject.cpp:123`) snaps any row whose translated world falls
outside the `[0, 0x3F·256]` tile extent back to the dungeon entrance. Safe, but those
graces lose their real position (cluster at the door). After #1/#2 land, revisit whether
the out-of-range cases are real data or a conv bug producing them — and whether a wider
valid extent (or the correct page) removes the need to clamp.

---

### TL;DR priority
1. **Measure M/T live (P/M keys) and bake** — fixes the global rotation/offset (do first).
2. **Transitive conv loop** — fixes Raya Lucaria / Liurnia / Ashen regions.
3. **Open-page RE** — stop cross-page bleed (needs a small read-only RE: page-id field).
4. Out-of-range clamp review — cosmetic spread, after 1–3.
