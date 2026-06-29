---
name: resolve-loot-flag-dlc-bug
description: "resolve_loot_flag's >=0x40000000 repeatable-cut wrongly drops DLC one-time loot; fix = live EventFlagMan group query — RUNTIME-VALIDATED 2026-06-25 (div=1000, 506/506 DLC kept). Wire BOTH L4145+L4207."
metadata: 
  node_type: memory
  type: project
---

`goblin::resolve_loot_flag` (goblin_inject.cpp ~L4111) classifies an ItemLotParam
`getItemFlagId` as repeatable/temp via `flag == -1 || flag >= 0x40000000 → return 0`
(the comment calls it "the empirical temp range — golden runes/crafting/consumables").
Used for collected-graying, census, AND the EMEVD/enemy/sibling notability filters.

**The cut has a real false-positive on the SOTE DLC (datamined 2026-06-25,
tools/probe_flag_ranges.py over ItemLotParam_map/_enemy):**
- ALL base-game persistent flags are **< 0x40000000** (Smithing Stone [1] `0x3e9030ec`,
  Grave Glovewort [1] `0x3e903100`; Rune Arc flag 0). So the cut catches **ONLY DLC flags**
  — 556 lots, all in high bytes **0x79 / 0x7A** (~2.04e9–2.05e9).
- Within that DLC range, **one-time and repeatable flags are INTERLEAVED, NOT numerically
  separable**: Royal Magic Grease `0x7a0990a0` (one-time) sits 80k below Cerulean Sea
  `0x7a085adc` (repeatable); Crucible Hammer Helm `0x79eb8218` (one-time). So **no threshold
  fix is possible** — the cut drops legit DLC one-time loot (equipment, key items, greases)
  along with the DLC repeatables (Shadow Realm Rune, crafting goods).

**Impact:** mainly **collected-graying + census** — DLC one-time loot never grays when
obtained and isn't counted as a tracked collectible (the cut returns 0 = "no persistent
flag"). Also makes the EMEVD/enemy no-bake passes drop ~2–5 DLC one-time sub-lots (Crucible
Hammer Helm, Royal Magic Grease for emevd; Blessed Bone Shard ×2 + Iris of Occultation for
enemy) → they stay baked (no loss, but not de-baked). See [[handoff-loot-from-real-files]].

**Proper fix = the LIVE persistent-flag query, not a value range** (the ground truth, per
`docs/re/windows_collected_loot_flag_re_findings.md`): a flag is persistent iff its GROUP is
allocated in the persistent EventFlagMan — `group = flagId / *(u32*)(mgr+0x1c)`,
`rbtree_lookup(mgr+0x38, group) != null`. AOBs in that doc: `FUN_1405f9400` (group bit-lookup),
`FUN_1405f9bf0` (IsEventFlag), manager layout (`[mgr+0x1c]` divisor, `mgr+0x38` group RB-tree).
NOTE: plain IsEventFlag(flag) does NOT distinguish them — an unobtained one-time flag reads
false just like a repeatable one; you need the GROUP-allocated check specifically. Separate
RE workstream (find the EventFlagMan singleton + port the group lookup). Probe:
tools/probe_flag_ranges.py.

**▶ NEXT-SESSION BRIEF (self-contained):** `docs/re/resolve_loot_flag_dlc_fix_prompt.md` — full
plan: add `flag_group_persistent(flagId)` (port FUN_1405f9400's std::map::find) + a factored
`flag_is_repeatable()` predicate, wire into **BOTH** numeric-cut sites — `resolve_loot_flag`
(goblin_inject.cpp:4145) AND `lot_row_in_table` (goblin_inject.cpp:4207, the notability resolver).

**✅ RUNTIME-VALIDATED 2026-06-25 (live RPM, `<ghidra_scripts>\flag_group_persistent.py`):**
- Use the **PRIMARY** `EVENT_FLAG_MAN_SLOT` AOB — it IS in the build, resolves the right manager
  (`divisor (mgr+0x1c) = 1000`). **NOT `_ALT`** (wrong singleton, reads +0x653c, divisor=551 garbage).
- Tree = `std::map<uint group, Block>` @ mgr+0x38: node key@+0x20, left@0, right@+0x10, isnil@+0x19;
  walk == `std::map::find(flagId/1000)`; **node exists ⇒ persistent**.
- Sweep of all 5496 nonzero loot flags: **DLC 506/506 persistent** (bug fixed, all kept); **base
  4979/4990** (only 11 absent → proves groups are **pre-allocated at save load, not lazy** → query
  reliable). The 11 base flips (keep→drop) are ERR custom/placeholder lots (groups 53000/59930/
  59931/59990/99997) the game doesn't persist — correct to drop.
- **SHIPPED + in-game confirmed 2026-06-25** (commit c244ac9): emevd de-bake rose **512 → 514**
  (the 2 DLC sub-lots Crucible Hammer Helm + Royal Magic Grease now notable); census/flag_or_pairs
  stay 0.00 ms (memoized group walk = no measurable cost). Pending only the visual graying check.
  NOTE: the recurring `eldenring.exe +0x1EB9999` 0xC0000005 in MapForGoblins_crash_*.txt is
  Elden Ring's KNOWN SHUTDOWN/TEARDOWN crash on Exit / Alt+F4 (ER frees its resources badly) —
  fires AFTER normal play (log runs fine to session end), identical on the old DLL, NOT our bug,
  nothing actionable. Ignore these crash files unless one points into MapForGoblins.dll.
- **CORRECTION: Cerulean Sea `0x7a085adc` is ONE-TIME, not repeatable** (old label was a name-based
  guess). The REAL repeatable signal is **`getItemFlagId == 0`** (Mushroom/Dewgem/Glovewort/Lily/
  Shadow Realm Rune all have flag 0, handled by the `resolved==0` path — never hit the persistence
  test). So Cerulean Sea is correctly KEPT now. Flag dumper: `tools/dump_loot_flags.py`.

**▶ PER-CATEGORY COLLECTED-FLAG PRECISION — VERDICT 2026-06-27 (the "100%-save over-report" follow-up;
DON'T re-investigate the per-slot patch).** The big over-report (temp/non-persistent flags reading false
forever — Golden Runes/Crafting/Consumables) is the cause-#1 above and is FIXED by the live group query.
The doc `windows_collected_loot_flag_re_findings.md` flagged a residual "unique-item cohort reads unset"
(Armaments/Talismans) and hypothesised the real flag is a **per-slot `getItemFlagId0N` (slot ≥2)** that
`resolve_loot_flag` misses (it reads lot-wide @+0x80, then slot-1 @+0x60 only for single-item lots).
**That patch is PROVEN INAPPLICABLE:** `items_database.json` has **ZERO multi-item lots containing an
armament or talisman** (all 31089 lots scanned) → every armament/talisman is single-item → the per-slot
scan beyond slot-1 can never fire for them. Evidence it's already healthy: the build-time
`[COVERAGE-CENSUS]` log (map_entry_layer.cpp ~L2418) shows `flagged ≈ drawn` in every loot category
(Armaments 317/317 flagged, Talismans 145/145) — NO cohort resolves to flag 0. The census<drawn gaps are
two CORRECT mechanisms: respawnable exclusion (Crafting 1109, Gloveworts 232 — flag-less by design) and
**flag-SHARING** (census counts DISTINCT flags; ERR shares one flag across set pieces — Armour 332→142,
Armaments 317→283 — the game itself can't disambiguate). Couldn't do the clean re-measure (<user> has no
true 100% ERR save); the `[CENSUS-UNSET]` line + `diag_loot_flags=true` ([LOOTDIAG] dumps all 8 slot
flags) are the tools IF a high-completion save appears. **Conclusion: residual = flag-sharing +
respawnable (both correct); no code change.**
