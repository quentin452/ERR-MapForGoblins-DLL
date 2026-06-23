# Findings — FieldIns pool anchor + geom→FieldIns join

Companion to `windows_fieldins_pool_anchor_and_join_re_prompt.md`. Partial: the RPM-determinable parts
are settled; STEP 1 (static anchor) and the join mechanism need Ghidra static analysis.

---

## STEP 2 (join) — RPM verdict: FieldIns NOT self-sufficient, and NO structural geom↔FieldIns link

Probed live on the same known chest **`AEG099_090_9000`** in `m60_37_50_00`, lot `1037500100`
(`0x3dd6fec4`), FieldIns relocated each run via the pool-node `{lotId, FieldIns*}` search
(`D:\ghidra_scripts\fieldins_pos_check.py`, `entityid_join_check.py`, `fieldins_link_check.py`).

| question | result |
|---|---|
| Does the FieldIns carry a world position? (self-sufficient → iterate pool only) | **NO** — searched FieldIns +0x600 and one pointer level for the chest's pos `(56.33,237.93,52.68)`; absent. FieldIns holds `lotId@+0x50` + inline name, no transform found. |
| Shared EntityID-shaped u32 between geom-side (`CSWorldGeomStaticIns`/`MsbPart`) and FieldIns? | **NO** — windows geom 0x800 / MsbPart 0x600 / FieldIns 0x1500; only matches were name-string fragments (`0x0030003x`) and noise (float 1.0, pointer half-dwords). |
| Direct or 2-level pointer geom_ins/MsbPart ↔ FieldIns? | **NO** — neither object points to the other, nor via one intermediate. |

**Conclusion:** the loaded-asset → FieldIns association is **not recoverable from the runtime
structures by proximity** (no shared key, no pointer). It is maintained by an **external
entity-keyed manager** (the Event.Treasure → gimmick registration). Recovering it needs **Ghidra on
the FieldIns spawn/registration path** (find the entity→FieldIns lookup), not RPM poking.

## STEP 1 (pool static base) — not done (needs Ghidra)
Today the pool is only reachable by heap-scanning for a known lotId (not anchorable). Finding the
owning singleton (`CSFieldInsMan`/gimmick FD4Singleton) + its static load site (`mov rcx,[rip+disp]`
→ `er_base+RVA`) and the singleton→pool→node-array chain is a Ghidra task. The pool RTTI is confirmed
`CS::CSGrowableNodePool<FieldInsBase*>`; node = `{lotId(u32)+flag, FieldIns*}`.

## Net status of the explore-cache supplement
- **Proven & usable now:** geom walk → loaded `(partName, pos)`; `resolve_loot_item_textid(lotId)` →
  item. So a cache that harvests geom positions and joins to **baked** `items_database` by `partName`
  works for all loot ALREADY in the bake — no new RE needed.
- **Blocked (needs Ghidra):** pairing a runtime position with a runtime `lotId` for **ERR-ADDED** loot
  (not in the bake) — that's the "added loot, with names" premium feature, and it needs both STEP 1
  (pool anchor) and STEP 2 (the entity→FieldIns join), neither resolvable by RPM.

Tools: `D:\ghidra_scripts\fieldins_pos_check.py`, `entityid_join_check.py`, `fieldins_link_check.py`.
