# Vision / decision note — embedded scripting API (C++ feature logic → scripts)

Status: **decision note, NOT a plan** (2026-07-02, from a user design discussion). Question raised:
should we tackle an embedded **scripting API** (move map-feature logic out of C++ into a script
language) sooner — to simplify hot-reload, make future map features cheaper, edit changes live, and
lay the premise of the framework in `runtime_modding_framework_vision.md`? **Decision below: not yet.**
Recorded so the direction + reasoning survive sessions. When any piece becomes real work, scope it as
a `docs/plans/` plan first.

Related: `runtime_modding_framework_vision.md` (the framework the user has in mind — but its shape is
a C++ core lib + client DLLs, NOT scripts), `plans/overlay_hot_reload_playwright_plan.md` (the
hot-reload we already have), `runtime_live_capabilities_audit.md` (what's proven live).

## The idea (as proposed)

Embed a script runtime (Lua / QuickJS / Wren / …). Write map features — build a marker category from
a param, a panel widget, a filter — as scripts interpreted at runtime. Edit a script file → reload the
script → change is live, with no C++ rebuild, no DLL swap, no game restart. Scripts become the "client
mods" running on the C++ "host" — the framework premise.

## Baseline: what already exists (what scripting would have to beat)

- **Render-layer hot-reload works today** (hot-reload Slice D, in-game validated on Linux/Proton):
  edit `goblin_overlay_render.cpp` → rebuild ONLY the render target → watcher auto-swaps in **~1.3 s**,
  game never restarts. "Edit overlay code and see it live" is already a solved, fast loop.
- **RPC live-drive** (`tools/mfg_rpc.py`): `screenshot`, `set <ini_key> <value>`, input injection,
  `reload_overlay`. Scripted/AI iteration against the running game is proven.
- **Live config**: `rebuild_markers()` already re-runs the bucket build on an F1 toggle without a
  restart; the whole ini is live-settable via RPC `set`.

So "see changes live" is largely already true — **for the render/overlay layer and for config**.

## The real pain this is aimed at (name it honestly)

Host / data-layer changes are NOT in the hot-swappable render module — `map_entry_layer.cpp` (param
parsing, `build_buckets`), `loot_disk`, the MSB/EMEVD parsers live in the **host**, which owns the
hooks and is not hot-reloaded. Editing them needs a **full game restart** (~40 s + Wine boot
flakiness). That is the friction (visible during the merchant-search work), and it is what scripting
is implicitly meant to remove.

## ROI, assessed against the four claimed benefits

| Claim | Reality | Verdict |
|---|---|---|
| Simplify hot-reload | Render hot-reload is already 1.3 s. Scripting extends *instant reload* to host feature-**glue** — but the heavy loops can't move (below) | Partial |
| Cheaper future features | Only for features that are **assembly of existing primitives**. New features almost always need new RE (param/MSB/EMEVD) → a new C++ primitive **and** a new binding. The binding always lags the feature | Modest |
| See changes live | RPC + render hot-reload already do this | Incremental |
| Framework premise | True direction — but the vision doc's shape is a **C++ core lib + client DLLs**, and it explicitly says *don't extract the framework until a 2nd mod exists*. Scripting is one framework shape, chosen speculatively | Speculative now |

## The hard costs

1. **Binding surface is enormous.** Value exists only if scripts can do what C++ does: `get_param`,
   parse MSB/EMEVD, project world→map, `push_marker`, draw ImGui, read event flags, resolve FMG names.
   That is ~110 `overlay_api` functions + the marker/param types, marshalled across the VM boundary and
   maintained forever.
2. **Hot paths stay C++.** `build_buckets` iterates thousands of param rows + a projection per build;
   the render loop is per-frame. Interpreted per-row/per-frame is too slow, so the biggest, most
   feature-dense file — `map_entry_layer.cpp` (~3.7 k lines, the actual feature core) — **does not move
   to script**. Scripting only gets the thin glue on top.
3. **It does not touch the hard part.** The work in every feature so far was **RE** — finding the
   param/flag/asset signal (portals, elevators, smithing tables, farmable, merchant stock). Scripting
   assembles primitives; it does not discover them. The bottleneck is RE, not assembly.
4. **It is a large project itself** (embed VM, design + version the API, bindings, marshalling, error
   handling, sandboxing, save/crash safety at the boundary) that delays real features while it is built.

## Decision — NOT YET (low ROI now, premature per the framework doc's own discipline)

Do not build an embedded scripting API now. It is binding maintenance for speculative flexibility, it
leaves the feature core and hot paths in C++ regardless, and it does nothing for the RE that is the
true bottleneck. This matches `runtime_modding_framework_vision.md`: *do not extract a framework
speculatively — wait until a second mod actually exists.*

## Higher-ROI moves that solve the actual pain sooner (and are stepping stones to the framework)

1. **Data-driven category/filter descriptor** (JSON/ini, read live). Declare "category X ← param Y,
   iconId Z, event-flag gate, section S" as **data**, not code. Covers a real slice of "new map feature
   = new landmark/loot category" with zero new C++ per category — ~80 % of the scripting benefit at
   ~20 % of the cost, and it is exactly where the no-bake direction already points. **This is the
   concrete near-term win to scope as a plan.**
2. **Improve the host-reload story.** Push more feature logic into the already-hot-reloadable render
   module where it fits, and/or make the data layer (`rebuild_markers()`) re-runnable against edited
   descriptors without a DLL swap — attack the 40 s restart directly.
3. **Keep the `overlay_api` / render boundary clean** (the framework doc's one standing action) so the
   C++ core *is* the eventual framework API — scriptable later, cheaply, once there is a real client.

## When scripting DOES become worth it (revisit triggers)

- A **repetitive feature-assembly pattern** is proven (the descriptor in move #1 keeps hitting its
  limits and clearly wants control flow, not just data).
- A **second mod / client** exists (the framework-extraction trigger from the vision doc).
- **Non-C++ contributors** want to add map features and the C++ build toolchain is their blocker.

Until at least one of these is true, the descriptor + host-reload work is the better spend.
