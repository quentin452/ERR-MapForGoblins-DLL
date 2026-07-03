# Param-override loader — plan (regulation.bin-free runtime param modding)

Status: **SCOPED 2026-07-03. Slice 1 helper PROTOTYPED (`src/goblin_param_edit.{hpp,cpp}`, not yet
wired to a loader or exported). Build-verification + in-game test pending.**

Origin: the architecture audit (`docs/runtime_live_capabilities_audit.md`) +
`docs/runtime_modding_framework_vision.md` + `docs/scripting_api_roi_note.md`. This is the FIRST
concrete step of the "mod Elden Ring without shipping a regulation.bin" direction, chosen ahead of
the vision doc's literal battle order for the reasons below.

## The thesis (why this first)

**Params reload from `regulation.bin` on every boot → editing a param FIELD in RAM is save-safe by
construction.** The edit evaporates on restart and the DLL re-applies it. That is *exactly what
Smithbox / a modified regulation.bin does* — but in memory, touching zero game files, composable with
any overhaul (no merge wars). A boot-time param-override loader is therefore:

- the minimum viable "mod ER without regulation.bin",
- **save-safe** (touches no persisted state — unlike item grants / event-flag writes),
- built almost entirely from PROVEN bricks (`get_param<T>`, live field writes, `resolve_field_offset`),
- mod-agnostic (reads/writes the ACTIVE install's live param tables),
- shippable as a standalone rebalance mod, AND the substrate every later gap (row-add B, items C)
  builds on.

This maps to **Gap A** ("write generic param field") in the capabilities audit, promoted to first
because it is independently shippable and the atom of everything downstream. Gap **D** (FMG inject) is
useless alone (renames nobody can edit); Gap **B** (mass row-add) is unproven (table realloc+resort);
Gap **C** (item grants) writes custom IDs into `.sl2` and MUST wait on policy **H** (reserved IDs).
None of those block this loader — it writes only live param fields.

## Proven precedents reused (file:line, checked-out)

- **Live field write on a param row:** `goblin_kindling.cpp:128` `safe_write_byte` via
  `WriteProcessMemory` (clang-cl ELIDES `__try` around plain stores — must use WPM/RPM, an opaque
  kernel call the optimizer can't drop; same reason all cross-memory access in the repo does).
  `goblin_kindling.cpp:650/659` flips a live row's `areaNo @ +0x20` between hidden/original.
- **Typed param access by name:** `from/params.hpp:219` `get_param<T>(L"Name")` →
  `ParamTableSequence<T>`; `:154` `try_get(id)` → raw row base pointer (T only drives the cast; a
  1-byte T still yields the row base, so a generic writer needs no per-param struct).
- **Self-correcting field offset from the exe:** `modutils::resolve_field_offset`
  (`modutils.hpp:57`, used at `goblin_item_classify.cpp:315`) AOB-scans the game's own access
  instruction and extracts the disp8/disp32 — the offset is authoritative + patch-proof, with a
  pinned constant only as a logged fallback. This is the bridge to Slice 2 (name-addressed).
- **Table expand + add rows (for later B):** `goblin_tutorial_popup.cpp:135` (HeapAlloc bigger table,
  recopy + templates, relayout locators, swap `file_ptr`/`num_rows`). Out of scope here; noted as the
  Gap-B precedent.

## Slices

### Slice 1 — generic `param_set_field` helper — PROTOTYPED
`src/goblin_param_edit.{hpp,cpp}`. Offset-addressed (caller passes the byte offset + width):

```cpp
namespace goblin::paramedit {
enum class FieldType { U8, S8, U16, S16, U32, S32, F32, F64, U64, S64 };
bool  param_set_field(const wchar_t* param, uint64_t row_id, ptrdiff_t off, FieldType t, double v);
std::optional<double> param_get_field(const wchar_t* param, uint64_t row_id, ptrdiff_t off, FieldType t);
}
```

- Writes through `WriteProcessMemory` (never a raw store — SEH-elision safety, matches the repo rule).
- `get_param` throws when a param is absent → wrapped in try/catch, returns `false`/`nullopt`.
- `double` is the transport for every numeric field; the writer narrows to the field width. Value
  range/precision loss for U64/S64 above 2^53 is a known limit (documented in the header; a raw-u64
  overload is a trivial add if a mod needs it).
- `param_get_field` is the read-back used to verify a write landed.
- **Save-safe:** writes only into the live in-RAM param table; nothing persists.

**Acceptance (Slice 1):** call `param_set_field(L"EquipParamWeapon", <id>, <off>, F32, x)` after param
load, read it back, and see the game reflect the value (e.g. weapon AR / goods effect changes live).
Mod-agnostic test: same call on vanilla + a non-ERR mod hits that install's own row values.

### Slice 2 — name-addressed offsets (paramdef-driven) — the multiplier
Turn `field_offset` into `field_name`. Two tiers:
1. **Cheap/interim:** per-field `resolve_field_offset` AOBs for the handful of fields a first override
   file targets (reuses `goblin_item_classify.cpp`'s pattern verbatim). Enough to author the demo file
   by name for known fields.
2. **Industrial (Phase-3 class 1, `nobake-endgame-roadmap.md`):** load PARAMDEFs at runtime (shipped
   defs / from regulation) and resolve ANY field offset by name → zero manual offsets, version-proof.
   This is the same "kill the manual offsets" endgame already on the roadmap; the override loader is
   its first real consumer.

Authoring an override by byte offset is a footgun (silently wrong after a patch) — **do not ship a
user-facing override file addressed by raw offset.** Slice 2 (at least tier 1) gates the public loader.

### Slice 3 — boot-time override loader — the shippable feature
`param_overrides.json` (or an ini section) next to the DLL, applied once params are ready (reuse the
existing param-poll readiness gate, `docs/HANDOFF.md` boot section):

```json
[ { "param": "EquipParamWeapon", "row": 1234, "field": "attackBasePhysics", "value": 200 },
  { "param": "EquipParamGoods",  "row": 110,  "field": "refId_default",     "value": 5 } ]
```

- Applied at boot AND re-appliable live (wire into a reload path like `rebuild_markers()` / an RPC
  command for the dev loop).
- Unknown param / row / field → logged + skipped, never fatal (prime-directive robustness: a file
  authored against a DIFFERENT mod's params degrades gracefully).
- Gate behind an ini flag (default OFF) since it MUTATES game balance — distinct from the read-only
  overlay. Reuse the existing `questAllowFlagWrite`-style cheat-gate discipline.

### Non-goals / boundaries
- **No item grants, no inventory writes, no event-flag writes** — those need policy H first (below).
- **No row ADD / table resize** — that is Gap B (unproven mass-add); this loader edits existing rows.
- **No scripting VM** (`scripting_api_roi_note.md` — deferred).
- Does not touch the mod-agnostic sourcing: still the ACTIVE install's live params.

## Parallel prerequisite to FREEZE (not a blocker for this loader)

**Policy H — reserved high-ID range + "DLL-required-at-load" contract.** No doc pins it yet (only
mentioned in `runtime_modding_framework_vision.md`). It is a POLICY decision, cheap, and it gates Gap C
(item grants → custom IDs written into `.sl2` → orphan-ID save corruption without a reserved range).
This param-field loader does NOT need it (edits no persisted state), so ship the loader first — but
decide + write H down before ANY inject-into-inventory work starts.

## Downstream (order after this lands)
1. **Slice 2 tier 2** (runtime paramdef) — retires manual offsets repo-wide, not just here.
2. **Gap D** generic `inject_fmg_entries` — pairs with param edits to rename/redescribe.
3. **Gap B** `param_add_rows` — generalize the TutorialParam expansion; validate on a
   non-id-looked-up table first; budget the heap test.
4. **Gap C** end-to-end item — AFTER H frozen; prototype on ONE item.
5. **Gap E** reactive event layer over the existing write-flag + observer primitives.

Framework discipline (from the vision doc): do NOT extract a `GoblinFramework` core until a 2nd mod
actually exists; keep the `overlay_api` / render boundary clean so the C++ host IS the eventual API.
