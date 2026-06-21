# RE findings — correct per-item "obtained" flag for loot markers (collected/census)

Answers `docs/re/windows_collected_loot_flag_re_prompt.md`. Static Ghidra (`D:\ghidra_proj2\ER`,
script `find_lootflag.java` / `find_lotdraw.java`) + Paramdex. App 2.6.2.0 / ERR 2.2.9.6.

---

## 0. TL;DR

Two distinct causes behind the 100%-save over-report, ranked:

1. **Temp / event-local flags read false forever (the big one).** `IsEventFlag`'s bit-lookup
   (`FUN_1405f9400`) maps `flagId → group = id/[mgr+0x1c]`, `bit = id%[mgr+0x1c]`, then red-black-
   tree-looks-up the group; **a group not present in the persistent (save-backed) manager ⇒ returns
   false, with no special-casing of large ids.** ER's temporary/event-local flags live in groups
   that manager never holds, so a `getItemFlagId` in that range is **never** readable as "collected".
   These lots are **repeatable / non-unique loot** (the flag is intentionally non-persistent so the
   lot can re-grant). The worst "broken" categories prove it: **Golden Runes 219/223, Crafting
   538/542, Consumables 172/178** — all repeatable/consumable. They are *not* once-only collectibles
   and must be **excluded** from census/graying, not counted as remaining.
   → Fix: in `resolve_loot_flag`, if the resolved flag is `-1` ("always droppable", Paramdex) or in
   the temp/non-persistent range (empirically `≥ 2³⁰` per the `[CENSUS-UNSET]` data), return "no
   persistent obtained flag" → the caller skips that marker for collected/census.

2. **Offsets are CORRECT — not the bug.** The current `resolve_loot_flag` offsets match the engine:
   `+0x80` lot-wide `getItemFlagId`, `+0x60` `getItemFlagId01`, `+0x04` `lotItemId02`, row size
   `0x98` (the **blocked** ITEMLOT_PARAM_ST layout: ids[8]@0x00, cats@0x20, basePoints@0x40,
   cumulate@0x50, perSlotFlags@0x60, lotWideFlag@0x80). ⚠️ The Paramdex `ItemLotParam.xml` web copy
   renders a *DS3-style interleaved* layout (rarity@0x00, flag@0x04, stride-0x14 slots) — that is
   **wrong for ER**; the `0x98` row size and the engine reads confirm the blocked layout.

For the residual **unique-item** cohort that still reads unset (Armaments 247/276, Talismans
111/139): these have a real persistent flag, so the wrong field is being returned per row — the
fix needs one runtime diagnostic (below). The temp-flag exclusion (#1) does not cover these.

---

## 1. `IsEventFlag` — the reader (Q2, confirmed)

`read_event_flag` → `FUN_1405d1330(mgr, &flagId)` (this is the SAME fn the fog reveal test uses):
`if (flagId != 0) return FUN_1405f9400(mgr, flagId) != 0; else false;`

```c
bool FUN_1405f9400(mgr, uint flagId) {
  uint group = flagId / *(uint*)(mgr+0x1c);     // group index
  uint bit   = flagId % *(uint*)(mgr+0x1c);     // bit within the group block
  node = rbtree_lookup(mgr+0x38, group);        // red-black tree of allocated groups
  if (!node) return false;                      // ← group absent ⇒ false (no high-id guard)
  block = (node.kind==1) ? node.idx*mgr[0x20] + mgr[0x28]   // inline block
        : (node.kind==2) ? node.ptr : 0;        // pointer block
  return block && (block[bit>>3] & (0x80 >> (bit&7)));
}
```
So **any** flag whose group isn't allocated in the persistent manager reads false. Temp/event-local
flags (the `≥2³⁰` cohort) are exactly that → permanently "not collected". This is the mechanism, not
a value-range check we can read statically; treat `≥2³⁰`/`-1` as non-persistent at the mod layer.

`SetEventFlag` = `FUN_1405d2240(mgr, &flagId, on)`: writes the persistent bit (`FUN_1405f9bf0`) **and**
a second manager (`FUN_140c9de20`), and (on first ON) fires a notify hook (`FUN_140d1e880`). Reads
only ever consult the persistent one, consistent with the above.

---

## 2. Icon-hide vs item-obtained (Q3)

The native map-icon predicate `FUN_140d58470(pointIns, slot, mode)` reads the point's
`WorldMapPointParam` row (`pointIns+0x10`) per-slot fields at `+0x30/+0x3c/+0x48/+0x54/+0x60/+0x6c/
+0x78/+0x84` (the `textEnableFlagId/textDisableFlagId` pairs) and shows the icon iff
`_VerifyEnableEventFlag` passes AND `_VerifyDisableEventFlag` fails. Both verifiers
(`FUN_140d58640`/`FUN_140d58550`) test their flag list through the **same** `IsEventFlag`
(`FUN_1405d1330`), AND-reduced over the present flags.

⇒ `textDisableFlagId*` is a **map-icon gate**, evaluated as an ordinary event flag. In vanilla it is
usually the same flag the treasure's `ItemLotParam.getItemFlagId` sets on pickup (icon hides ⇔ item
taken), but it is a *separate field* — for a randomizer/ERR remap the two can diverge, which is why
the baked `textDisableFlagId1` fallback can be the wrong "obtained" flag.

---

## 3. The obtained flag (Q1) + recommendation

`getItemFlagId` (lot-wide `+0x80`) is the canonical "lot has been drawn / item obtained, do not
re-grant" field (Paramdex/community semantics; `-1` = always re-droppable). So the resolution order
is right in spirit; the fixes are:

```text
flag = row[+0x80]                                  # lot-wide getItemFlagId
if flag == 0 and row[+0x04]==0:  flag = row[+0x60] # single-item → slot-1 getItemFlagId01
if flag == 0:                    flag = baked textDisableFlagId1
# NEW guards:
if flag == -1 or flag >= 0x40000000:  -> NOT COLLECTIBLE (repeatable/temp) — skip census+graying
```
The `≥2³⁰` guard removes the repeatable cohort (golden runes / crafting / consumables) that can
never read collected — the dominant part of the over-report.

**Residual unique-item cohort (needs 1 runtime check).** For armaments/talismans that are unique yet
read unset, the wrong field is returned per row. Cheapest resolution (quentin, 100% save): for ~5
known-obtained armament markers, log `lotId, lotType, row[+0x04], row[+0x60..+0x7C] (8×getItemFlagId0N),
row[+0x80]` and which of those `read_event_flag` returns true for. That pinpoints whether the real
taken-flag is a **per-slot `getItemFlagId0N`** (not just slot-1) or a **baked/live mismatch** (ERR
remap), so `resolve_loot_flag` can prefer the populated per-slot flag, or scan all 8 slots and use
the first whose flag is in the persistent range.

> The exact `CSItemLotMan` consume routine (which would name the field authoritatively) is not
> AOB-anchorable from the data we have — the `SetEventFlag` callers in the item band are Lua/menu
> message handlers, not the lot draw. The runtime diagnostic above is the direct substitute and is
> decisive on a 100% save.

---

## 4. AOBs / offsets — version-stability

| fn | role | AOB (entry) |
|---|---|---|
| `FUN_1405d1330` | `IsEventFlag(mgr,&id)` (also fog reveal test) | `48 83 EC 28 8B 12 85 D2 74 0F E8` |
| `FUN_1405f9400` | flag bit-lookup (group=id/[mgr+0x1c]) | `44 8B 41 1C 44 8B DA 33 D2 41 8B C3 41 F7 F0 4C 8B D1` |
| `FUN_1405d2240` | `SetEventFlag(mgr,&id,on)` | `48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 8B 12` |
| `FUN_140d58470` | WorldMapPointParam show predicate | `48 89 74 24 10 57 48 83 EC 20 4C 8B 51 10 48 8B F1 48 63 FA 4D 85 D2` |

- ITEMLOT_PARAM_ST (blocked, row `0x98`): `lotItemId01..08` `+0x00`(stride 4); `lotItemCategory01..08`
  `+0x20`; `lotItemBasePoint01..08` `+0x40`(u16); `cumulateLotPoint01..08` `+0x50`(u16);
  **`getItemFlagId01..08` `+0x60`(stride 4)**; **`getItemFlagId` (lot-wide) `+0x80`**; `cumulateNumFlagId`
  `+0x84`. (`+0x04` = `lotItemId02`.)
- EventFlagMan: `[mgr+0x1c]` = group divisor; `mgr+0x38` = group RB-tree; `mgr+0x20/0x28` inline-block
  base/stride. Reuse `goblin::ui::read_event_flag` (`IS_EVENT_FLAG` AOB) — it already calls the above.
- WorldMapPointParam show-flag slots: `row+0x30/0x3c/0x48/0x54/0x60/0x6c/0x78/0x84`.

Scripts: `D:\ghidra_scripts\find_lootflag.java`, `find_lotdraw.java` (outputs `out_lootflag.txt`,
`out_lotdraw.txt`).
