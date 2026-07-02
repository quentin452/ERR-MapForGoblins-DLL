# Data-driven category descriptor — plan

Status: **Tier 1 COMPLETE on `feat/category-descriptor` (2026-07-02) — steps 1, 2, 2b, 3, 4 all
landed + verified byte-identical.** The category descriptor `data/categories.json` is now the SINGLE
source of truth for **the enum itself, name, section, landmark iconIds, glyphs (numeric + name-keyed),
and the coverage display-name** — generated into `goblin_categories.gen.hpp` + `goblin_categories_enum.gen.inc`
(wired into `generate_data.py`). Every step proven identical to the code it replaced via an offline
diff against git HEAD (stronger than a count check — every mapping, not just totals). Commits:
`fd29ef2` (name+section), `8f0a340` (landmark icons), `2763c57` (glyphs), `80aec3c` (coverage),
`354d1e3` (enum). **Adding a category is now literally one `data/categories.json` record + regen.**
Steps 1+2 detail:
- **Step 1 (`fd29ef2`)** — category **name + section** are data-driven: `data/categories.json` (source
  of truth) → `src/generated/goblin_categories.gen.hpp` `CATEGORY_META[]` via
  `tools/generate_categories.py` (wired into `generate_data.py`). `category_name()` / `section_of()`
  are array lookups; a `static_assert` ties the table to the `Category` enum. 0 diffs vs the old
  switches across all 85 categories.
- **Step 2 (`8f0a340`)** — **landmark iconId→category** folded in (`landmark_icon_ids` per record →
  generated `landmark_category_for_icon()`; the C++ fn is a forwarder). 86/86 iconIds identical.

Step 2b (`2763c57`) + Step 3 (`80aec3c`) then folded glyphs + the coverage display-name in.
**Remaining: only the optional Step 4 (generate the `Category` enum itself) — the enum stays
hand-edited in `goblin_map_data.hpp`, guarded by the `static_assert` that fails the build if the table
and enum diverge; generating it removes the last hand-edit but risks reordering (the contiguous
landmark block + any serialized enum value), so it's deferred until wanted.**

**Edit surface for a new LANDMARK category now: the `goblin_map_data.hpp` enum entry + ONE
`categories.json` record (name, section, landmark_icon_ids, optional glyph) + regen — down from ~6
scattered switches.** Name/section/glyph/coverage all follow from the one record. (Non-landmark
categories with bespoke builders still add their C++ builder; the descriptor owns their metadata.)
The near-term win from `docs/scripting_api_roi_note.md` (chosen over a full scripting API).

**In-game smoke test — PASS (2026-07-02, ERR/Proton, the generated-enum build `7e7e3ecf`):**
`[LANDMARKLIVE] built 298` with all 16 landmark categories at their exact counts,
`[COVERAGE]` 81 categories with names intact (rendered from the generated CATEGORY_META), `[SIG]`
30/30 clean, 0 errors. Confirms the whole descriptor refactor (generated enum + name + section +
landmark map + glyphs) is behavior-identical. This is on top of the per-step offline byte-diffs
(names/sections 0 diffs, 86/86 landmark iconIds, 10+2 glyphs incl. computed tints, coverage doc
unchanged, 85/85 enumerator sequence identical).

## Goal

Make a marker CATEGORY a single data record instead of ~6 hand-edited C++/py switches. Adding a
landmark/param-filter category should be **one table edit + regen**, not a scavenger hunt across the
codebase. This is the "~80 % of the scripting benefit at ~20 % of the cost" move: it kills the
per-category boilerplate without a script VM, without moving hot loops out of C++, and without the
~110-fn binding surface. A later runtime tier (Tier 2) delivers the actual "edit a category live" dream
for the repetitive landmark pattern.

## The fragmentation today (what a new landmark category touches)

Adding e.g. "Churches" today means editing all of:

1. `src/generated/goblin_map_data.hpp` — the `Category` enum entry. **Hand-edited** (despite the
   `generated/` path — `generate_data.py` only *copies* this header to other bake dirs; there is NO
   generator for the enum today).
2. `src/worldmap/map_entry_layer.cpp` `landmark_category_for_icon()` — the `iconId → Category` switch.
3. `src/goblin_markers.cpp` `markers_category_name()` — the display-name switch.
4. `src/goblin_section_visibility.cpp` `category_section()` — the `Category → Section` switch.
5. `src/worldmap/category_meta.cpp` `CATEGORY_GPU_ICONS` — optional native glyph iconId + scale + tint.
6. `tools/coverage_vs_mapgenie.py` `SECTIONS` / `ENUM2DISPLAY` — coverage mapping.
7. `assets/lang/fr.txt` — the category-name translation.
8. (default on/off is already implicit via the section-toggle machinery.)

Six of these are pure metadata for ONE concept. The landmark families (WorldDivineTower ..
WorldUniqueSite, ~16 categories) are the cleanest case: they already differ ONLY by
`(iconId(s), enum name, display name, section, glyph)` — `build_live_landmarks()` is a single shared
pass keyed off `landmark_category_for_icon()`.

## Tier 1 — compile-time descriptor (single source of truth) — the near-term win

Author every category ONCE in a data table (`data/categories.json`, or a `tools/categories.py` list),
one record per category:

```
{ enum: "WorldChurch", section: "World", display_en: "Churches", display_fr: "Églises",
  landmark_icon_ids: [3, 20, 247, 248, 249],        // omit for non-landmark categories
  glyph: { iconId: 0, scale: 1.0, tint: null },      // optional native map glyph
  coverage: { mapgenie: ["Landmark"] },              // for coverage_vs_mapgenie.py
  default_on: false }
```

A generator (extend `tools/generate_data.py`, or a new `tools/generate_categories.py`) emits from that
table, replacing the hand-edited switches:

- the `Category` enum (into `goblin_map_data.hpp`), **preserving the contiguous landmark block** the
  code relies on (`build_live_landmarks` assumes `WorldDivineTower .. WorldUniqueSite` are contiguous;
  the generator must keep declared-order contiguity + emit a `static_assert` guard);
- `markers_category_name()` (name switch);
- `category_section()` (section switch);
- `landmark_category_for_icon()` (the iconId→category map);
- `CATEGORY_GPU_ICONS` (glyph table);
- the coverage `SECTIONS`/`ENUM2DISPLAY` (or have `coverage_vs_mapgenie.py` import the same table);
- `assets/lang/fr.txt` category entries (or leave translations hand-authored, keyed by the display_en).

Result: **add a category = edit one record + `python tools/generate_categories.py` + build.** No
runtime interpreter, no perf cost (all still compile-time C++), low risk (generated code is the same
shape as today).

Scope boundary: the descriptor owns **metadata + the shared landmark/param-filter build**. Categories
whose build logic is bespoke RE (disk-loot passes, EMEVD parses, enemy drops, summoning pools, portals)
keep their hand-written C++ builder — the descriptor still owns their enum/name/section/glyph/coverage,
but the builder stays code. The descriptor has an optional `builder: "custom"` marker for those.

Migration order (lowest risk first): (1) generator emits the enum + name + section for ALL categories
from the table, diffed byte-for-byte against today's hand-written output (no behavior change — pure
consolidation); (2) fold in `landmark_category_for_icon` + `CATEGORY_GPU_ICONS`; (3) fold in coverage;
(4) delete the now-dead hand-written switches. Each step is build + in-game "counts unchanged"
(`[LANDMARKLIVE]`) verifiable.

## Tier 2 — runtime descriptor (the "edit a category live" dream) — optional, later

A `categories.json` next to the DLL, read at boot (and re-read via the existing `rebuild_markers()`
path → no restart), that adds **landmark-pattern categories only** — `iconId`-filtered
WorldMapPointParam rows into a reserved DYNAMIC-category tail beyond the compile-time enum, drawn with
the native glyph for that iconId (or the universal circle). This is the genuinely live-editable slice,
and it's exactly the repetitive pattern users would want to extend.

Cost (why it's Tier 2, not 1): `Category` is a compile-time `uint8_t` enum used as an array index
(`g_buckets` is `std::array<vector<Marker>, NUM_CAT>`). Dynamic categories need `g_buckets` to carry a
dynamic tail (fixed array + a `vector` of dynamic buckets), and the section list, the F1 category
tree, the search, and the coverage all have to iterate `NUM_CAT + dynamic` instead of a constant.
That's a real refactor of the category-index assumption — do it only once Tier 1 proves the field set
and a user actually wants runtime-added categories (a revisit trigger from the scripting note).

## Acceptance (mod-agnostic test)

- Tier 1: regenerating from the table produces byte-identical behavior to today (same enum, same
  `[LANDMARKLIVE]` counts, same panel/coverage). Adding a landmark category is one record.
- Tier 2: dropping a `categories.json` line for a new iconId adds a live, toggleable, searchable
  category on ANY install with no rebuild; unknown iconIds draw the circle fallback (prime directive).

## Non-goals

- Not a scripting VM (see `scripting_api_roi_note.md` — deferred).
- Does not move bespoke RE builders (loot/EMEVD/enemy) into data — those stay C++; the descriptor only
  owns their metadata.
- Does not change the mod-agnostic runtime/disk sourcing — categories still read the ACTIVE install's
  live params.
