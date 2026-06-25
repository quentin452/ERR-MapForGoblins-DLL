# RE TASK — fix `resolve_loot_flag` to not drop DLC one-time loot (live persistent-group query)

> **Self-contained brief for a future session.** Branch `feat/msbe-entity-recover-dummy`.
> Read alongside `windows_collected_loot_flag_re_findings.md` (the original flag RE) and the
> memory `resolve-loot-flag-dlc-bug`. Datamined + scoped 2026-06-25; deferred for context size.

## The problem (proven)

`goblin::resolve_loot_flag` (`src/goblin_inject.cpp` ~L4111) decides whether an
`ItemLotParam.getItemFlagId` is a **persistent one-time** flag (track it: graying + census +
the EMEVD/enemy notability filters) or a **repeatable/temp** flag (ignore it). It uses a
NUMERIC heuristic:

```cpp
if (resolved == 0xFFFFFFFFu || resolved >= 0x40000000u)   // L4145
    return 0;   // treat as repeatable → not a tracked collectible
```

**This wrongly drops legit SOTE-DLC one-time loot.** Datamined over ItemLotParam_map/_enemy
(`tools/probe_flag_ranges.py`):

- **ALL base-game persistent flags are `< 0x40000000`** (Smithing Stone [1] `0x3e9030ec`,
  Grave Glovewort [1] `0x3e903100`; Rune Arc flag 0). The `>= 0x40000000` cut therefore catches
  **only DLC flags** — 556 lots, all in high bytes **0x79 / 0x7A** (~2.04–2.05e9).
- Within that DLC range, **one-time and repeatable flags are INTERLEAVED → NOT numerically
  separable**:
  - `0x79eb8218` Crucible Hammer Helm — **one-time** (DLC armour)
  - `0x7a0990a0` Royal Magic Grease — **one-time**
  - `0x7a085adc` Cerulean Sea — **repeatable** (sits *between* the two one-times)
- So **no threshold/range fix is possible.** The cut drops DLC equipment, greases, key items
  (Iris of Occultation, Blessed Bone Shard, …) along with the DLC repeatables.

**Impact:** mainly **collected-graying + census** (DLC one-time loot never grays when obtained,
isn't counted). Secondary: the no-bake passes drop a few DLC one-time sub-lots (~2 emevd:
Crucible Hammer Helm + Royal Magic Grease; ~3 enemy: Blessed Bone Shard ×2, Iris of Occultation)
→ they stay baked (no loss, but not de-baked).

## The fix (ground truth = live persistent-group query, not a value range)

Per `windows_collected_loot_flag_re_findings.md` §1, persistence is decided by whether the
flag's GROUP is allocated in the persistent `EventFlagMan` red-black tree:

```c
// FUN_1405f9400(mgr, flagId)  — the game's flag bit-lookup:
uint group = flagId / *(uint*)(mgr + 0x1c);     // group divisor @ mgr+0x1c
node = rbtree_lookup(mgr + 0x38, group);        // group RB-tree @ mgr+0x38
if (!node) return false;                         // group absent ⇒ NON-persistent (repeatable)
... // else read the bit (block @ node.kind: inline mgr[0x20]/mgr[0x28], or node.ptr)
```

**A flag is persistent (one-time) IFF its group node EXISTS in that tree.** That is the exact
signal — NOT the bit value. (Plain `IsEventFlag(flag)` returns false for an *unobtained* one-time
flag too, so it can't be used to classify; you need the **group-allocated** check specifically.)

### Implementation plan

1. **New runtime helper** `bool goblin::flag_group_persistent(uint32_t flagId)`:
   - Resolve the `EventFlagMan` singleton — REUSE the existing resolve. `src/re_signatures.hpp`
     has `EVENT_FLAG_MAN_SLOT` / `EVENT_FLAG_MAN_SLOT_ALT`; `goblin_markers.cpp` + `goblin_kindling.cpp`
     already resolve `g_is_event_flag` + the manager (see `read_event_flag` → `orp_flag_set`,
     `goblin_inject.cpp:4016`). Lift the manager pointer the same way.
   - `divisor = *(uint32_t*)(mgr + 0x1c)` (cache it; constant). `group = flagId / divisor`.
   - `rbtree_lookup(mgr + 0x38, group)` → return `node != nil`. The tree is an MSVC `std::_Tree`
     (`std::map<uint, Block>`): **decompile `FUN_1405f9400` in Ghidra to read the exact node
     offsets** (standard layout: `_Left@0x00, _Parent@0x08, _Right@0x10, _Color@0x18(byte),
     _Isnil@0x19(byte)`, key `_Myval` ~`@0x20`; head/root via the container's `_Myhead`). Port the
     iterative walk (root = head→_Parent; descend by key compare; stop at `_Isnil`). Validate the
     offsets live (RPM) before trusting them — see the validation oracle below.
   - All reads are RPM/guarded (clang-cl elides `__try` on raw loads — see memory
     `build-toolchain-clang-xwin` gotcha #5; use ReadProcessMemory or the existing safe-read
     helpers, NOT raw `*ptr`).
2. **Wire into `resolve_loot_flag`** (`goblin_inject.cpp` ~L4136-4147): replace the numeric cut
   with the live query, keeping cheap short-circuits + a safe fallback:
   ```cpp
   if (resolved == 0)            return baked_flag ? ... : 0;  // keep existing 0 handling
   if (resolved == 0xFFFFFFFFu)  return 0;                     // -1 = always re-droppable (keep)
   // live ground truth: a flag whose group isn't persistent is repeatable
   if (g_flagman_ok && !flag_group_persistent(resolved)) return 0;
   // fallback when the manager isn't resolved yet: keep the old numeric heuristic so behaviour
   // never regresses pre-resolve (or on a profile where the manager AOB misses).
   if (!g_flagman_ok && resolved >= 0x40000000u) return 0;
   return resolved;
   ```
3. **Performance/threading:** `resolve_loot_flag` is called per-marker (census/graying, ~hundreds
   per refresh) AND on the disk-build worker thread concurrently with render-thread callers.
   - Memoize: a thread-safe `flagId → persistent?` cache (or cache by `group`), guarded like the
     existing `LotReader` `std::call_once` inits. The rbtree walk is O(log n) but per-marker adds up.
   - The manager pointer + divisor resolve once (call_once), same pattern as `s_lots`.

## Validation oracle

- **Offline (no game):** `tools/probe_flag_ranges.py` (the flag structure) +
  `tools/datamine_emevd_residual.py` (the resolve_loot_flag SIM + the uncovered-residual dump).
  After the fix the runtime should additionally cover Crucible Hammer Helm (lot 2045470401) +
  Royal Magic Grease (lot 2047440901).
- **Specific test flags** (the helper MUST classify these correctly):
  | flag | item | expected |
  |---|---|---|
  | `0x79eb8218` (2045477400) | Crucible Hammer Helm | **persistent** (keep) |
  | `0x7a0990a0` (2047447200) | Royal Magic Grease | **persistent** (keep) |
  | `0x7a085adc` (2047367900) | Cerulean Sea | **repeatable** (drop) |
  | `0x3e9030ec` (Smithing Stone [1]) | base-game | **persistent** (keep) |
  | `0` / `0xffffffff` | — | drop (unchanged) |
- **Runtime:** `loot_emevd_drops=true` + `diag_loot_pos` → `[LOOTDISK] emevd drops … replaced N` should
  rise past 512/529 (the 2 DLC equipment now de-baked). Also re-check the census/graying over-report
  (`docs/re/windows_collected_loot_flag_re_findings.md` §1 was tuned around the numeric cut — confirm
  the live query doesn't re-introduce the 100%-save over-report it fixed: repeatable DLC goods like
  Shadow Realm Rune / Cerulean Sea must STILL drop).

## Files

- `src/goblin_inject.cpp` — `resolve_loot_flag` (~L4111) + the new `flag_group_persistent`.
- `src/goblin_inject.hpp` — declare `flag_group_persistent`.
- `src/re_signatures.hpp` — `EVENT_FLAG_MAN_SLOT*` already present; add a sig only if the rbtree
  walk needs a dedicated sub-fn (likely not — replicate the walk in C++).
- Reuse: `goblin_markers.cpp` / `goblin_kindling.cpp` (manager + IsEventFlag resolve),
  `goblin_inject.cpp:4016` `read_event_flag`/`orp_flag_set`.

## Why deferred

Real but bounded (DLC one-time tracking; ~2 emevd + ~3 enemy lots for the no-bake). The EMEVD
de-bake is already 512/529 (97%) without it. This is its own RE workstream (manager rbtree walk),
split out to keep the EMEVD-pass session focused. See memory [[resolve-loot-flag-dlc-bug]] +
[[handoff-loot-from-real-files]].
