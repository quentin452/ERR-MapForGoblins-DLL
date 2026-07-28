# F1 settings search — make the UI index itself (step 2, DEFERRED)

**Status: PROPOSAL, not started.** Step 1 shipped 2026-07-28 and fixed the reported bug; this
records step 2 so the analysis is not lost. Nothing here is decided.

## The defect that started it

Typing **"Divine Tower"** into the F1 `find settings…` box found nothing — while
`World - Divine Towers` is a real category with a real toggle (`show_divine_towers`,
`goblin_config_schema.cpp`). Searching **"world"** *did* surface it. That asymmetry is the whole
story.

## Root cause — the search indexes hand-written strings, not the UI

Every panel section gates itself on a keyword string it declares by hand:

```cpp
const bool show_seccat = f.match("sections categories marker show hide cluster world");
```

`Filter::match` (`panel_util.cpp`) token-matches the query against **that string only**. So the
searchable surface is exactly "what a developer remembered to type", and everything data-driven is
invisible: ~60 generated category labels, and the human descriptions the config schema already
carries per setting.

This is not an isolated oversight — `panel_internal.hpp` documents it as the design ("Each block
calls match() with its keyword…"), and there are ~18 such call sites. It is the
complexity-ceiling shape `CLAUDE.md` names: **a duplicate index maintained by hand, which must grow
in lockstep with every feature and drifts silently the moment someone forgets.** The reported bug is
its normal failure mode, not an accident.

Compounding it: **three search boxes with three scopes** (settings `f`, `##catfilter`,
`##itemsearch`) and nothing telling the user which covers what — so a scope miss reads as "broken".

## Why "just search the UI automatically" is not free

ImGui is **immediate mode**: there is no retained widget tree to walk. Worse, the ordering is
against us — a section decides whether to draw *before* its widgets run, so at filter time the
labels it contains are not yet known.

## Step 1 — SHIPPED 2026-07-28

`panel_categories.cpp` now also matches the **live** section/category labels
(`overlay_api::section_label` / `category_label`) with the same token semantics as `Filter::match`,
and when a label is what matched it pushes the query into the inner `##catfilter` (only on a *change*
of the outer query, so typing in the inner box is never clobbered). Landing on the section is not
finding the row.

Covers the reported bug and every category, present and future, for free — the list's own data is the
index. It does **not** touch the ~18 other sections.

## Step 2 — the proposal (NOT started)

**Invert the roles: labels become the automatic index, keyword strings become an optional synonym
supplement.** Two independent pieces:

1. **Config-backed settings** — `goblin::ini_schema()` (`goblin_config_schema.cpp:748`) is already
   enumerable and already carries a description per entry (`"Divine Tower locations (iconId 23)"`).
   Index it. Every setting becomes findable by the words the user actually reads.
   ⚠ open question: the schema's INI sections do **not** map 1:1 onto panel sections, so "which panel
   section hosts this setting" has no answer today. That mapping is the real work, not the matching.
2. **Hand-written widgets** — the rest are inline labels inside an already-filtered block:
   ```cpp
   ImGui::Checkbox(tr("Show the compact HUD in game"), &config::runHud);   // today
   f.checkbox("Show the compact HUD in game", &config::runHud);            // proposed
   ```
   The widget self-filters on its own label; a section header draws only if a child drew. The
   header-before-children ordering is solvable with `ImDrawListSplitter` (already in the build).

**What this buys:** the failure mode inverts. Today, forgetting a keyword makes a setting
**unreachable**. After, forgetting one costs a *synonym* — the setting is still findable by its own
name.

**What it costs, stated honestly:**

- it touches every section file (~18 `f.match` sites) — a real refactor, not a patch;
- the current strings carry **synonyms that appear nowhere on screen** (clustering declares
  `declutter dense threshold distance`). Pure-automatic would lose those, so the keyword string must
  survive as an optional supplement — the goal is to stop it being the *sole* index, not to delete it;
- piece 1 is blocked on the schema↔panel-section mapping above.

## Decision needed before starting

Is the F1 settings search worth a cross-cutting refactor, or is step 1 (plus ad-hoc label indexing
in whichever section next reports a miss) enough? Step 1 already covers the largest data-driven
surface. Reasonable to leave this deferred until a second miss is reported in a *different* section —
that would be the signal that the pattern, not the instance, is the problem.
