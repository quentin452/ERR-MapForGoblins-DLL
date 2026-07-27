---
name: legacy-fold-negative-grid-wrap
description: "legacy_fold::fold computed its intermediate block with (uint8_t)(wx/256.0), which WRAPS to 255 for a negative wx — any sub-map marker with a local coord under -256 broke its fold chain and landed on the map's far corner. Clamped 2026-07-27."
metadata:
  node_type: memory
  type: project
---

# The fold's intermediate grid wrapped on negative coordinates

**Symptom.** 19 markers stacked at the exact same world point `(65352, 65373)` = grid `(255,255)` — the
"unset" sentinel, the far corner of the map. All `srcArea 35` (Ashen Capital). Found by the spoiler-log
audit's out-of-bounds counter ([[spoiler-log-marker-audit]]).

**One line, in `legacy_fold::fold`:**

```cpp
cgx = (uint8_t)(wx / 256.0);   // wx = -307 → -1.199 → -1 → 255
```

A sub-map local coordinate under **-256** makes `wx/256.0` negative; truncation gives -1 and the cast to
`uint8_t` wraps it to **255**. The next `lookup()` finds no block near grid 255 (the nearest-base fallback
caps at distance 6), so the loop **breaks after a single hop**. The out-of-range guard below cannot
rescue it either: `ent` is still the FIRST hop's dst base, which for row 12 is `(-184, -163)` — a
legitimate in-frame coordinate in Leyndell's space, but negative as a world coordinate. Snapping to it
keeps wx negative, `floor(-184/256) = -1`, and the final `(uint8_t)` wraps to 255 again.

Fix: **clamp, never cast**. A negative wx simply means "left of tile 0", so block 0 is correct.

## The previous diagnosis was wrong — don't trust it

`docs/re/dungeon_entrance_anchor_re_probe_findings.md` filed this as **Class C**: "a vanilla sub-map
dead-end that SHOULD lift via `legacy_fold`'s `reverse_lookup` (m19/m34/m35) but still declines". That is
not what happens:

- area 35 **is a src** — row 12 `(35,0,0) → (11,0,0)` — so it is NOT a dead-end and `reverse_lookup` is
  correctly skipped (it only fires for areas with ZERO forward rows);
- the onward chain exists too — row 108 `(11,0,0) → (60,45,52)`;
- so `35 → 11 → 60` folds perfectly **once the intermediate grid stops wrapping**.

## Numbers (vanilla + Randomizer v0.11.4)

Simulated over all 9828 m35 placements before touching the code:

| | sentinel (255,255) | real tiles |
|---|---|---|
| `(uint8_t)` cast (before) | **2864** | 44-45 / 49-51 |
| clamped (after) | **0** | 44-45 / 49-51 |

2864 is exactly the number of placements with a local X or Z under -256 — a 1:1 correlation, which is
what made the mechanism certain rather than plausible. Live after the fix: **out-of-bounds 19 → 0**, the
19 landed on real Leyndell tiles, area-35 marker count unchanged (117), and **one extra marker survived**
in area 12 (`WorldStakesOfMarika` 204 → 205) — two markers had been colliding on the sentinel cell, so
the cell-dedup had been silently deleting one.

## Method note worth keeping

The first simulation of this fold **clamped negatives to 0** and produced a perfect result, which read as
"the code is fine". Only reproducing the C++ cast's exact semantics (truncate toward zero, then wrap)
exposed the bug. When simulating suspect code, model **what it does**, not what it ought to do.
