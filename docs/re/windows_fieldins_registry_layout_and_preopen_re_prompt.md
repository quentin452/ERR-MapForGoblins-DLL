# RE prompt — FieldIns registry exact layout + the pre-open residency question (Ghidra)

> Two in-process RPM probes settled what RPM can; the rest is Ghidra. **(1)** The embedded per-asset
> pool at `instance+0x3A8` is EMPTY at tile-load (loot FieldIns not parented on the asset until later).
> **(2)** The documented global registry chain `er+0x3d7b0c0 → [+0x10] → +0x720 → map+0x8 …` walked as
> an `std::map<u64,ptr>` reaches only ~2 nodes in-process — it does NOT reproduce the
> hundreds/thousands-entry field-instance registry. So the asset→lotId link is not reachable in-process
> with the current offsets. Need Ghidra to deliver the items below. App = current ERR build; re-anchor.

See `windows_fieldins_pool_anchor_and_join_re_findings.md` for the full runtime results + the STEP-1
static anchor work (commit be1b018) this builds on.

---

## ★ THE MAKE-OR-BREAK QUESTION (answer FIRST — the whole feature hinges on it)

**Does a treasure's `lotId`-bearing `FieldIns` exist RESIDENT BEFORE the chest is opened, or only
after the player opens it?**

- Path A proved the loot FieldIns is **not** on the asset instance at tile-load.
- If the lotId-bearing FieldIns is created **only when the chest opens** → an explore-cache can only
  identify loot the player has ALREADY opened = near-useless (defeats "reveal added loot as you walk
  in"). **Kill the feature, keep baked-only**, and say so.
- If it IS resident from tile-load (just not where path A/B looked) → the feature is alive; deliver the
  reach below.

How to settle it in Ghidra/CE: find the FieldIns/treasure ctor (`FUN_1406c5900` builds the embedded
pool; the loot FieldIns ctor is its child) and trace WHO calls it and WHEN — at MSB
`Events.Treasure` parse / tile-load, or from the chest-open/interact path. Cross-check in CE: stand at
an UN-opened known chest (`AEG099_090_9000`, lot `1037500100`/`0x3dd6fec4`), search for the lotId; is
a FieldIns (vt in the `FieldInsBase` family, lotId@+0x50) already present, or does it only appear after
the open animation?

---

## STEP 1 (re-confirm) — the registry container + EXACT node layout

The in-process walk of `sub+0x720` as `std::map<u64,ptr>` reached 2 nodes, so something is off. Nail:
- From the static slot `er+0x3d7b0c0`: confirm `reg = [er+0x3d7b0c0]`, the `[+0x10]` sub-object, and
  whether `+0x720` is the right field — decompile the per-frame iterator `FUN_140b32d00` and the
  add/remove fns `FUN_140b32880`/`FUN_140b32b90`/`FUN_140b32010` and read the ACTUAL container base
  + node layout they use. Is it a `std::map` (`_Myhead` ptr + `_Mysize`), or an FD4 tree/hash/array
  with a different header? Where exactly are head, root, and the left/parent/right/key/value fields?
- Deliver: the corrected chain (offsets) from `er+0x3d7b0c0` to the iterable node set, the node stride
  + field offsets (left/right/value), and the live element COUNT field (so the mod can size + bulk-read
  it, not tree-walk under Wine).

## STEP 2 — the treasure FieldIns vtable + lotId offset (the filter)

The registry holds many `FieldInsBase` subclasses (we saw a `MapIns` vt `0x2a8f650` with `+0x50 =
0xffffffff` = no-lot; one unrelated lot-bearing obj vt `0x30308e8`).
- Identify the **treasure/item-pickup FieldIns subclass vtable** (RVA) so the mod can filter the
  registry to just loot-bearing instances. Confirm `lotId` is at `+0x50` on THAT subclass (it may
  differ per subclass — the `0xffffffff` we read on MapIns suggests `+0x50` is not universally lotId).
- Confirm whether that FieldIns also carries a **world position** (so the registry alone gives
  pos+lotId+name = self-sufficient, no geom join) or whether we still must join to the geom instance —
  and if so, by what key (EntityID on both? a back-pointer?).

---

## Deliverable — `windows_fieldins_registry_layout_and_preopen_re_findings.md`
1. **Pre-open residency verdict** (resident-from-load = feature alive / open-time-only = feature dead).
2. Corrected registry chain from `er+0x3d7b0c0` + node layout + count field (for a bulk read).
3. Treasure FieldIns vtable RVA + confirmed lotId offset on it + whether it carries position (and the
   join key if not).
4. If any of these is a dead end (open-time-only, or no stable anchor), state it definitively → the
   explore-cache stays **baked-partName-join only** (which already works for baked loot today) and the
   "ERR-added-loot-with-names" premium is shelved.

## Notes
- Reuse: STEP-1 static anchor work + AOBs already in `windows_fieldins_pool_anchor_and_join_re_findings.md`.
- The mod side is ready to wire the instant the chain+vtable+offset are correct (see the
  `diag_fieldins_join` probe in `goblin_collected.cpp` for the read pattern).
- Wine: deliver a COUNT so the mod bulk-reads the node array; do not make it tree-walk per-node.
