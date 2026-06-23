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

**The in-DLL hover route is approximate** (cursor render coord, exact only if perfectly
snapped). For the EXACT bake, hook the placement at the source: see
**`docs/windows_marker_affine_hook_re_prompt.md`** — a Windows/Cheat-Engine RE brief that
bp's the build/reconcile call site (NON-VMP), reads `world_in` from `point+0x80` and
`render_out` from the icon's written matrix → clean `(world→render)` pairs → lstsq `M`+`T`,
zero hover error. (Approximate cursor-hover variant: `docs/marker_mapspace_CT_recipe.md`.)

**Risk / possible deeper RE:** if `M` does NOT come out shared across pages, or residuals
stay large, the transform is more than one global affine (per-region origins inside a
page, or a non-affine Scaleform step) — the hook brief says to report the per-page `M`s.

## 2. Legacy conv — ✅ ESSENTIALLY DONE (investigated 2026-06-20)

The "transitive multi-hop" worry is a **non-issue**: the Python pipeline already resolves
the chains. `generate_legacy_conv_cpp` (`tools/generate_data.py`) has a `resolve()` that
composes every chain (e.g. Ashen Capital m35 → area 11 → area 60) down to 60/61 at bake
time — so **every baked `LEGACY_CONV` `dst_area` is 60 or 61**. A runtime loop = dead code.
The area-16 "wrong region" bug was the offset-drop, already fixed by the base-point
translation (`f960f81`).

Coverage audit of the 562 graces (231 native 60/61, 178 native 12/40-43, 153 legacy):
- conv keys on `src_gx`; `src_gz` is 0 everywhere and `src_gx` is unique per area → no
  collision. Of 153 legacy graces, **148 hit an exact base-point**.
- **5 imperfect, all degrade to an entrance cluster (visible, region-ish):**
  - area 19 (1 grace, Haligtree) — genuinely absent from `WorldMapLegacyConvParam` (no
    overworld surface position; the game itself has none). Leave un-projected → it'll be
    handled by the open-page filter (#3), not the overworld.
  - (10,1)×1, (31,8)×3 — gridX matches no conv entry of the area → fall back.

**Shipped:** the fallback was the *first* entry of the area (arbitrary far entrance); now
the **NEAREST base-point by grid distance** (`goblin_inject.cpp` `project_dungeon_row_to_
overworld`) — keeps those 4 at the closest dungeon mouth. Exact match now also checks
`src_gz` (was `src_gx` only). Nothing more to do here until #1/#3 land.

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
1. **Measure M/T (placement-hook RE) and bake** — fixes the global rotation/offset. THE one.
2. ✅ Legacy conv — done (pipeline flattens chains; nearest-entrance fallback shipped).
3. **Open-page RE** — stop cross-page bleed (needs a small read-only RE: page-id field).
4. Out-of-range clamp review — cosmetic spread, after 1–3.
