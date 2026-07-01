# Plan — extract subsystems out of `goblin_inject.cpp` (biggest hand-written god file)

**Status:** PR 0 DONE (2026-07-01, branch `feat/inject-module`), builds clean
(clang-cl+xwin, Linux dev-alt toolchain), IN-GAME CONFIRMED via log check (2026-07-01 19:20:
[SIG] 29/29 PASS, diag_loot_flags producing correct live output, no crash/error). Ready to
merge to master (not yet merged). PRs 1-4 not started. Modeled
on the already-landed [input_module_refactor_plan.md](input_module_refactor_plan.md) precedent
(`feat/input-module`, merged 2026-07-01, pure relocation + thin accessor plumbing, no logic
changes).

**PR 0 finding (revises the table below):** the line-by-line audit found `LotReader` is NOT
shared with `inject_map_entries` as its own comment claimed — that comment described a CALL
relationship (the functions that use it are themselves called from the map-entry-injection flow),
not shared instantiation; every `LotReader` use is a function-local `static` inside 5 functions,
all already textually adjacent. So **PR 0 and PR 2 collapsed into one clean extraction** —
new files `src/goblin_loot_resolve.cpp` (LotReader + `encode_live_item` + the whole
"Live persistence classifier" section, which turned out to exist ONLY to support
`flag_is_repeatable`/`LotReader`'s 5 consumer functions — moved together, not split) +
`src/goblin_inject_shared.hpp` (3 tiny accessors: `orp_flag_set`, `orp_manager_resolve_tried`,
`orp_event_man_slot_ptr` — the ONE genuine cross-boundary dependency found, since
`flag_query_persistent` piggybacks on the kill-indicators section's already-resolved
EventFlagMan slot rather than re-resolving its own). Also duplicated two trivial helpers
(`icon_rpm_i32`/`icon_rpm_ptr`, `NUM_CATEGORIES`) into the new file rather than reaching into the
icon-harvest block for them — same per-file-copy convention the codebase already uses for small
AOB/RPM helpers (`goblin_markers.cpp`/`goblin_kindling.cpp` each keep their own). `classify_item_live`/
`npc_loot_lot`/`aeg_pickup_lot`/`goods_is_map`/`npc_team_and_name` (taxonomy-based, NOT LotReader-based)
remain unmoved — genuinely separate concern, still a real future PR (renumber as PR "next" below).

## Why

`src/goblin_inject.cpp` is 5266 lines — bigger than `goblin_overlay.cpp` (3962, already partially
split via the input-module extraction) and more mixed: ~11 distinct subsystems bolted into one
translation unit (family-group visibility, marker clustering, grace anchors, player world/marker
position, item classification, TutorialParam injection, a ~1600-line icon-texture RE/harvest/GPU
registry block, native grace suppression, overlay control API, kill indicators, live-persistence
classifier, live-loot hide). Every declaration lives in one 563-line facade header
(`goblin_inject.hpp`) with zero existing split. Same problem the input-module refactor solved:
finding/touching one subsystem means reading past ten others first.

## Ground truth (verified 2026-07-01, before writing this plan)

- **Build system:** CMake, explicit source list (no GLOB) — `CMakeLists.txt:139-166`,
  `add_library(MapForGoblins SHARED ...)`. New split files need entries there, same slot the
  input-module split used.
- **Public surface:** everything is declared in ONE header, `goblin_inject.hpp` (563 lines,
  `namespace goblin` + nested `namespace goblin::ui`) — every function declared there is
  implemented in `goblin_inject.cpp`. A split needs per-module headers; decide per-module whether
  `goblin_inject.hpp` stays as an include-everything facade (lower call-site churn) or gets
  replaced by direct includes at call sites (cleaner, more diff) — check caller count before
  choosing, don't guess.
- **Header precedent to follow:** `src/input/input_shared.hpp` — a tiny accessor-only header
  exposing read-only getters for state that STAYS in the original file, with each extracted piece
  getting its own `.hpp`/`.cpp`, all re-included back where needed. Reuse this shape, not a new one.
- **Real coupling found (the reason this can't be a mechanical 1:1 section→file split):**
  - **Visibility (`:186`) and marker-clustering (`:411`) are NOT independent.** `:356-365`
    explicitly documents `g_clusters_expanded`/`g_cluster_member_ptrs` living inside the visibility
    block specifically so `is_section_hidden_ptr` can read clustering state; both share
    `g_section_hidden_mtx`. **Move these two as ONE unit**, not split.
  - **`LotReader` (anonymous namespace `:1069`) is cross-cutting infrastructure, not icon-specific
    or classification-specific** — its own comment states it's shared by `inject_map_entries`
    (elsewhere in this file) AND the loot-refresh path. It sits textually inside the icon-harvest
    span but isn't icon content. **Extract this into its own small shared unit FIRST**, before
    splitting either the icon block or the classification functions that also need it — otherwise
    both later extractions end up depending on whichever one happens to keep it.
  - The icon-harvest block's other 3 anonymous namespaces (`:1753` CreateImage capture context,
    `:2923` bind-test/force-bind state, `:4004` grace-suppression hook) ARE confined to their
    respective sub-pieces — no extra shared-header needed for those three.
  - Item/loot-classification-live functions (`classify_item_live`, `npc_loot_lot`,
    `aeg_pickup_lot`, `lot_row_in_table`, `lot_item_count`, `goods_is_map`, `npc_team_and_name`)
    are declared interleaved with icon declarations in the header (proximity, not relatedness) but
    are a semantically distinct concern (no-bake item/loot classification, not icon harvesting) —
    scope as its OWN extraction unit, don't bundle with icons just because they're declared nearby.
- **Not yet audited to PR-boundary precision** (flag for a targeted second pass before finalizing,
  same rigor as the visibility/clustering finding above): player world/marker position (`:729-1128`),
  item classification (`:1128-1417`), TutorialParam (`:1417-1746`), native grace suppression
  (`:3998-4040`), overlay control API (`:4130`–end), kill indicators (`:4432`), live-persistence
  classifier (`:4587`), live-loot hide (`:4689`). Boundaries and rough content are known; a
  line-by-line file-static/cross-section-read audit (the kind done for visibility/clustering) is
  NOT done for these — do it before committing to their exact split, the way the input-module
  plan's own "Scope audit" pass caught `hk_present`'s hidden coupling before that refactor started.

## Scope — sequencing, biggest/cleanest win first

| PR | Content | Depends on | Size (~lines) | Risk / Status |
| --- | --- | --- | --- | --- |
| **0. `LotReader` + live-persistence classifier + 4 consumer fns + diag** ✅ DONE | `src/goblin_loot_resolve.cpp` (new): `LotReader`, `encode_live_item`, `ANON_LABEL_TEXTID`, the whole "Live persistence classifier" (`flag_query_persistent`/`fg_read_node`/`flag_is_repeatable`), `resolve_loot_flag`, `resolve_loot_item_textid`, `lot_row_in_table`, `lot_item_count`, `diag_loot_flags`. `src/goblin_inject_shared.hpp` (new): 3-fn accessor surface for `orp_flag_set` (stays in `goblin_inject.cpp`, genuinely shared with other sections there). Declarations UNCHANGED in `goblin_inject.hpp` (facade kept, lower call-site churn). | — | 425 lines moved | DONE + IN-GAME CONFIRMED 2026-07-01, `feat/inject-module`, builds clean (clang-cl+xwin), ready to merge |
| **1. Icon-texture harvest/GPU registry** | `:1746`–`:4130`ish (now shifted ~366 lines up post-PR0, re-grep before starting): icon probe, image enumerate, harvest-via-find-hook, proactive repo-walk harvest, TWIN-map walk, CreateImage force-bind, central GPU-icon registry, OodleLZ_Decompress hook, item-icon XML layout | PR 0 landed | ~1500-1600, the single biggest chunk | medium — self-contained per research, but not yet line-audited for OTHER cross-section reads beyond the 4 anonymous namespaces found |
| **2. Item/loot classification-live (taxonomy-based)** | `classify_item_live`, `npc_loot_lot`, `aeg_pickup_lot`, `goods_is_map`, `npc_team_and_name` — confirmed during PR 0's audit to be a SEPARATE concern from `LotReader` (taxonomy/goodsType-based, not ItemLotParam-row-based); `lot_row_in_table`/`lot_item_count` already moved in PR 0, drop them from this PR's scope | — | small-medium | low-medium, not yet re-scoped post-PR0 |
| **3. Visibility + marker-clustering (as ONE unit)** | `:186`–`:411`ish + grace anchors `:428`–`:729`ish | — | medium | medium (shared-mutex coupling already mapped, but still the riskiest single move since two "sections" become one file) |
| **4. Remaining sections** (player position, item classification `:1128`, TutorialParam, native grace suppression, overlay control API, kill indicators) | Second-pass audit first (see Ground truth above), THEN scope exact PR boundaries — do not batch all of these into one commit | PRs 0-3 landed + audit done | large in total, unknown per-piece until audited | unknown until audited — don't guess a table entry here |

Land 0→1→2→3 in order (0 is a hard prerequisite for 1 and 2). PR 4 gets its own scoping pass —
same discipline as this plan's own Ground-truth section — before any code moves.

## Design sketch

- One `.cpp`/`.hpp` pair per extracted unit, same shape as `src/input/*` — each owns its own file-
  static state and diagnostics, exposes an init/registration call if it installs hooks (icon block
  hooks `CreateImage`/`OodleLZ_Decompress`), consumed from wherever `goblin_inject.cpp` currently
  wires it.
- Shared cross-cutting state stays in a small accessor header (`input_shared.hpp` precedent) —
  do NOT re-own shared globals into whichever file happens to move first; expose them via a getter/
  setter header both sides include.
- `goblin_inject.hpp`'s fate (facade vs direct includes) is decided PER MODULE at PR time, based on
  actual caller count for that module's declarations — not decided up front for the whole file.

## Explicit non-goals

- Pure relocation — no logic changes, no fixing any of the RE findings/hacks documented inline in
  these sections (e.g. the icon-harvest block's `§8`/`§8b`/`§8c` RE citations stay as-is, just move
  file).
- Not touching `goblin_overlay.cpp` itself — that file's remaining god-file trim is scoped
  separately in [overlay_hot_reload_playwright_plan.md](overlay_hot_reload_playwright_plan.md)
  Phase 1 (extracting ImGui draw functions, a different motivation: hot-reload, not just size).
- Not re-deriving or second-guessing any RE conclusion documented in the moved code — if a comment
  cites a `docs/re/*` finding, that citation moves with the code unchanged.

## Suggested order

1. Fork `feat/inject-module` (or similar) from `master` when ready to start.
2. PR 0 (`LotReader` shared extraction) first, build+smoke-test — this unblocks PRs 1 and 2 and is
   the lowest-risk move in the whole plan.
3. PR 1 (icon-harvest), build+deploy+smoke-test in-game (icon rendering is directly observable —
   easiest PR in this plan to catch a regression in, don't skip the live check).
4. PR 2 (item/loot classification), build+smoke-test — verify against
   `docs/memory/tooling/item-classification-guard.md`'s `[ITEMCLASS]` census diff, same regression
   guard already used for classification changes.
5. PR 3 (visibility+clustering as one unit), build+smoke-test — this is the riskiest move
   (shared-mutex coupling), test toggling section visibility AND clustering together after the move.
6. Audit + scope PR 4 as its own follow-up pass, do not start it in the same session as 0-3 without
   re-reading this plan's "not yet audited" caveat first.
7. Update `docs/memory/tooling/` with the new file map once any PR lands (mirror
   `docs/memory/tooling/input-hooks.md`), so future sessions know where each subsystem now lives.
