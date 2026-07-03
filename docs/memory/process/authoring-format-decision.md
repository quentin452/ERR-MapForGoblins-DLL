---
name: authoring-format-decision
description: "LOCKED 2026-07-03 — framework mod-authoring is DATA-FIRST, tiered: C++ (primitives/RE/hot loops) → data (declarations: ini for flat, JSON for rich records like custom items) → scripting (Lua, DEFERRED until a mod needs real control flow). Pick the lowest layer that expresses the mod."
metadata:
  node_type: project
  type: project
---

**Locked decision (2026-07-03, user).** Answers "JSON vs C++ vs Lua vs other for the framework's
mod-authoring surface?" Extends [[framework-regulation-agnostic-decision]] + the earlier
`docs/scripting_api_roi_note.md` (scripting deferred) + `docs/plans/category_descriptor_plan.md`
(data descriptor chosen) to the authoring FORMAT. Don't re-litigate.

## The rule — a 3-layer spectrum; pick the LOWEST layer that expresses the mod

| Mod must express | Layer | Cost |
|---|---|---|
| new RE / a new primitive / a hot per-frame or per-row loop | **C++** | rebuild; not user-authorable |
| static DECLARATIONS ("add item, set stat, rename, category X ← param Y") | **Data (JSON/ini)** | ~zero (a parser) |
| LOGIC — conditionals, loops, event reactions, computed values | **Scripting (Lua/JS)** | HUGE (VM + ~110-fn binding surface, maintained forever) |

Data if it's a declaration; script ONLY if it needs control flow; C++ for the primitive underneath.
Never jump a layer up until the one below genuinely can't say it.

## Where the framework is (all declarative → data)

Everything the shipped primitives do is a declaration, no logic:
- param override = "row R, field F = V" (`param_set_field`)
- custom item (Gap C) = "clone template T → id N, name '…', set fields {…}, icon …"
  (`param_clone_row` + `param_set_field` + `inject_fmg_entries`)
- FMG rename = "slot S, id I = text"

→ **Data is the authoring format. No scripting needed now.** `param_overrides.ini` proves it end-to-end.

## ini vs JSON (the sub-choice)

- **ini (mINI, already linked)** — fine for FLAT `key = value` config. Keep it for that. Caveat: mINI
  LOWERCASES keys → case-sensitive identifiers must go in the VALUE (the `Param:row:field:value` hack
  in `param_overrides.ini`). Ugly for anything nested.
- **JSON** — right the moment records get nested/listy. A custom item is a record with a fields map:
  `{ "clone":"EquipParamGoods:1074000", "id":90000001, "name":"…", "fields":{ "sortGroupId":101 } }`.
  **When Gap C (custom items) lands, add a small JSON parser** (the DLL has none yet; mINI stays for
  flat config). JSON is the default — ubiquitous + the `data/` pipeline already speaks it. TOML is a
  fine human-nicer alternative (also needs a lib).

## Scripting (Lua) — DEFERRED, explicit revisit triggers

Do NOT build a VM speculatively (the ROI note's conclusion: binding maintenance for speculative
flexibility; hot loops + RE stay C++ regardless). Revisit ONLY when a mod needs logic you can't
express as declarative **trigger→action data** (e.g. "if player has X and flag Y, grant Z at a
computed value"). Reopen triggers: (1) a repetitive assembly pattern that clearly wants control flow,
(2) a SECOND mod / non-C++ contributor (also the framework-extraction trigger). If justified →
**Lua** (small, embeddable, the ER-modding world already speaks it). C++-core + data-glue until then.

## Recommendation (frozen)

Data-first, tiered: ini for flat config (keep), **JSON for rich records (add at Gap C)**, C++ for
primitives/RE/hot loops, scripting deferred with the trigger above.
