---
name: map-point-glyph-ids
description: "Visually-confirmed SB_MapCursor[_02] glyph → MENU_MAP_<NN>/name mapping for map-point icons (grace, martyr effigy, NPC). Derived with tools/menu_tex_extract, not derivable from params."
metadata:
  node_type: memory
  type: reference
---

Map-point glyph identities, confirmed by EYE (2026-06-30) using `tools/menu_tex_extract` (offline Oodle
decompress of `mod/menu/hi/01_common.tpf.dcx` → DDS → PNG) cross-referenced against the `[MAPPTLAYOUT]`
rects parsed by `parse_map_point_layout` (goblin_inject.cpp). These are NOT derivable from params/csv — the
WorldMapPointParam.csv Name col is empty and the ERR massedit iconIds (374+) point to glyphs that do not
exist in any current menu file (numeric glyphs cap at 261). So this visual map is the source of truth.

| Use | Glyph | name/NN | sheet | rect (x,y,w,h) |
|-----|-------|---------|-------|----------------|
| Grace (discovered) | gold sigil | `MENU_MAP_01_Bonfire` | SB_MapCursor_02 | 718,524,156,156 |
| Summoning Pool | Martyr Effigy (crucified figure) | `MENU_MAP_89` | SB_MapCursor_02 | 792,916,106,176 |
| Quest NPC (framed hood) | NPC | `MENU_MAP_80` (+ `MENU_MAP_80_Add` small) | SB_MapCursor_02 | 554,364,124,124 |
| NPC (full silhouette, alt) | bare hood | `MENU_MAP_86` | SB_MapCursor_02 | 0,1008,366,370 |
| Summon figures (alt) | 2 figures on platform | `MENU_MAP_21` | SB_MapCursor | 1296,182,194,234 |

NOTE: `SB_MapCursor_ERR.png` is referenced as an imagePath by 8 `MENU_MAP_ERR_*` layout entries but no such
standalone texture was found in 01_common.tpf by the extractor — those names likely resolve within
SB_MapCursor / _02. Confirm before relying on an ERR-only map sheet.

These feed the planned mod-agnostic disk map-point render path: native_map_point_icon reads the rect from the
ACTIVE install's SB_MapCursor[_02] (disk via Oodle) — the glyph art comes from whatever mod is loaded; only
the NN index is fixed here. Bonus consumers: (1) grace discovered/undiscovered state (replace green check),
(2) WorldSummoningPools → 89, (3) quest NPCs on the feature/quests branch → 80. See [[category-icons-00solo-atlas]].
