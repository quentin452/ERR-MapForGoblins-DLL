# RE TASK — fix `resolve_loot_flag` to not drop DLC one-time loot (live persistent-group query)

> ✅ **IMPLEMENTED 2026-06-25 (commit c244ac9).** `flag_query_persistent` + `flag_is_repeatable`
> in `goblin_inject.cpp`, wired into both `resolve_loot_flag` (L4145) and `lot_row_in_table` (L4207).
> Built (build-clang) — pending in-game runtime test (deploy blocked while the game is running).
> Kept below as the design record + runtime-validation log.

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

> **⚠ The same numeric cut lives in TWO functions** — both must be fixed or the bug only
> half-goes-away:
> 1. `goblin::resolve_loot_flag` — `goblin_inject.cpp:4145` (graying/census/de-bake flag).
> 2. `goblin::lot_row_in_table` — `goblin_inject.cpp:4207` (the **notability** resolver, comment
>    literally reads *"same semantics as resolve_loot_flag … repeatable/temp range → not notable"*).
>    This is the only OTHER `>= 0x40000000` in `src/` (`grep -rn 0x40000000 src/` = exactly these 2).
> Route BOTH through the new helper.

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

## ✅ RUNTIME-VALIDATED 2026-06-25 (live RPM, eldenring.exe running)

The whole mechanism below was **proven live** — script `D:\ghidra_scripts\flag_group_persistent.py`
(reusable; reads `D:\ghidra_scripts\loot_flags_dump.txt` from `tools/dump_loot_flags.py`).

- **Manager resolve:** the **PRIMARY** `EVENT_FLAG_MAN_SLOT` AOB
  (`48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9`, rip-disp `{{3,7}}`) **is present in this build**
  and resolves the correct manager. ⚠ **Do NOT use `EVENT_FLAG_MAN_SLOT_ALT`** — it resolves a
  *different* singleton (reads `+0x653c`; gave `divisor=551`, garbage map → every lookup wrong).
  The PRIMARY gives a clean **`divisor (mgr+0x1c) = 1000`** and a valid `_Myhead (mgr+0x38)`.
- **Offsets confirmed live** (std::map<uint group, Block>): node `_Left@0, _Parent@+0x08,
  `_Right@+0x10`, `_Isnil@+0x19 (byte)`, key `group @ +0x20 (uint)`. The walk == `std::map::find`:
  `group = flagId/1000`; lower_bound then exact-key test; **group node exists ⇒ persistent**.
- **Sweep over all 5496 distinct nonzero loot flags:**
  - **DLC (≥0x40000000): 506/506 persistent, 0 absent** → the live query KEEPS every DLC one-time
    flag. **Bug fixed**, and no nonzero DLC flag is wrongly kept-as-repeatable.
  - **base (<0x40000000): 4979/4990 persistent, only 11 absent.** Near-100% presence on a
    mid-progress save **PROVES persistent groups are pre-allocated at save load, NOT lazily per
    area** — so "group absent ⇒ non-persistent" is reliable (no lazy-allocation false-drops).
- **⚠ Cerulean Sea is NOT repeatable — the brief's old label was WRONG.** It is the *only* "repeatable"
  in `probe_flag_ranges.py`'s hand-curated name list that has a **nonzero** flag; every genuine
  repeatable (Mushroom, Dewgem, Ghost Glovewort, Deep-Purple Lily, Shadow Realm Rune) has
  **`getItemFlagId == 0`** and is already handled by the existing `resolved == 0` path — it never
  reaches the persistence test. So the real signal is **flag 0 = repeatable**, and the live query
  correctly classifies Cerulean Sea (group 2047367 allocated) as **persistent/one-time**.
- **⚠ NEW caveat (the brief originally said base-game is unaffected — it isn't, slightly):** the 11
  absent base flags flip **keep→drop** vs the old numeric cut. They sit in 5 non-standard groups
  (53000, 59930, 59931, 59990, 99997 — "Veteran's Helm", "Cracked Pot", "Perfume Bottle", "Soft
  Cotton", …), none referenced anywhere in the repo → ERR custom/merchant/placeholder lots the
  game itself does NOT persist. Dropping them is correct (they're not trackable one-time loot), but
  **eyeball this list when wiring** in case a real one-time hides there.

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
2. **Factor the cut into ONE predicate, then wire BOTH call sites through it.** Don't inline the
   query twice — make a small `bool flag_is_repeatable(uint32_t flag)` that both `resolve_loot_flag`
   and `lot_row_in_table` call, so the fallback + memoization live in one place:
   ```cpp
   // returns true ⇒ treat as repeatable/temp (drop). Folds the old -1/0x40000000 short-circuits
   // and the live group query + safe pre-resolve fallback into one spot.
   bool goblin::flag_is_repeatable(uint32_t flag) {
       if (flag == 0xFFFFFFFFu) return true;              // -1 = always re-droppable
       if (g_flagman_ok)        return !flag_group_persistent(flag);  // live ground truth
       return flag >= 0x40000000u;                        // fallback: old numeric heuristic
   }
   ```
   - **`resolve_loot_flag`** (`goblin_inject.cpp:4145`), replace
     `if (resolved == 0xFFFFFFFFu || resolved >= 0x40000000u) return 0;` with
     `if (flag_is_repeatable(resolved)) return 0;` (keep the existing `resolved == 0` handling
     above it untouched).
   - **`lot_row_in_table`** (`goblin_inject.cpp:4207`), replace
     `if (flag == 0xFFFFFFFFu || flag >= 0x40000000u) flag = 0;` with
     `if (flag_is_repeatable(flag)) flag = 0;`.
   - Note `flag == 0` is NOT repeatable (Rune Arc is flag 0 = persistent); both sites already treat
     0 specially before the cut — preserve that, `flag_is_repeatable(0)` must return **false**.
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
  | `0x79eb8218` (2045477400) | Crucible Hammer Helm | **persistent** (keep) — live ✓ |
  | `0x7a0990a0` (2047447200) | Royal Magic Grease | **persistent** (keep) — live ✓ |
  | `0x7a085adc` (2047367900) | Cerulean Sea | **persistent** (keep) — live ✓ (NOT repeatable; old label was wrong) |
  | `0x3e9030ec` (1049637xxx, Smithing Stone [1]) | base-game | **persistent** (keep) — live ✓ |
  | `6001` / `120` | AlwaysOn / Prologue | **persistent** — live ✓ (sanity) |
  | `0` / `0xffffffff` | — | drop (unchanged; `flag==0` IS the real repeatable signal) |
- **Runtime:** `loot_emevd_drops=true` + `diag_loot_pos` → `[LOOTDISK] emevd drops … replaced N` should
  rise past 512/529 (the 2 DLC equipment now de-baked). Also re-check the census/graying over-report
  (`docs/re/windows_collected_loot_flag_re_findings.md` §1 was tuned around the numeric cut — confirm
  the live query doesn't re-introduce the 100%-save over-report it fixed: the true repeatables that
  must STILL drop are the **`flag==0`** goods — Shadow Realm Rune, Mushroom, Dewgem, Ghost Glovewort,
  Deep-Purple Lily — already dropped by the `resolved==0` path (NOT by the persistence test).
  Cerulean Sea is **one-time** and is now correctly KEPT — that is intended, not a regression).

## Files

- `src/goblin_inject.cpp` — the new `flag_group_persistent` + `flag_is_repeatable`, then wire
  **both** sites: `resolve_loot_flag` (L4145) AND `lot_row_in_table` (L4207).
- `src/goblin_inject.hpp` — declare `flag_group_persistent` + `flag_is_repeatable`.
- `src/re_signatures.hpp` — `EVENT_FLAG_MAN_SLOT*` already present; add a sig only if the rbtree
  walk needs a dedicated sub-fn (likely not — replicate the walk in C++).
- Reuse: `goblin_markers.cpp` / `goblin_kindling.cpp` (manager + IsEventFlag resolve),
  `goblin_inject.cpp:4016` `read_event_flag`/`orp_flag_set`.

## Why deferred

Real but bounded (DLC one-time tracking; ~2 emevd + ~3 enemy lots for the no-bake). The EMEVD
de-bake is already 512/529 (97%) without it. This is its own RE workstream (manager rbtree walk),
split out to keep the EMEVD-pass session focused. See memory [[resolve-loot-flag-dlc-bug]] +
[[handoff-loot-from-real-files]].
