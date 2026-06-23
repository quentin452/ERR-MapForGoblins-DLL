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

---

## ★ GHIDRA PASS (2026-06-23, scripts `find_fieldins.java`..`find_fieldins6.java`, raw out `D:\ghidra_scripts\out_fieldins{3..6}.txt`)

RTTI-walked the pool family and decompiled the FieldIns ctor/dtor + the owning singleton's registry.
Confirmed names: pool RTTI `CS::CSGrowableNodePool<CS::FieldInsBase*>` (vtable RVA `0x2a84ca0`, TD
`0x3c8d210`); siblings `<CSWorldGeomIns*>` `0x2a838c8`, `<CSWorldGeomDynamicIns*>` `0x2a838d8`,
`<MapIns*>` `0x2a8f640`. `FieldInsBase` vtable RVA `0x2a25e68` (vt0 `FUN_14062e5d0`), TD `0x3c7a1e0`.
Class chain: `FieldInsBase` ← `CSWorldGeomIns`/`MapIns` ← (`CSWorldGeomStaticIns` etc.).

### STEP 1 — SOLVED: static anchor + iterable registry of all loaded field instances
The FieldIns/geom/MapIns ctors (`FUN_1406c5900` treasure-asset ctor, `FUN_140719f10` MapIns ctor)
**self-register the instance into a global FD4Singleton-owned registry**, keyed by a 64-bit id built
from the instance's MSB-part data + `*(short*)(this+0x2c)`. The assert path literally cites
`...Core/Singleton/FD4Singleton.h`, so the owner is an `FD4Singleton`.

- **Static slot: `er_base + 0x3d7b0c0`** (per-frame field-instance step singleton; its dtor
  `FUN_140b1bf60` touches `CS::CSGraphicsStep`/`CSStepTask` vtables). Read by the per-frame world step
  `FUN_140623410` and dozens of field fns.
- **AOB to pin** (re_signatures.hpp convention = pin code, capture rip-disp@3 → slot RVA):
  - `48 8B 05 ?? ?? ?? ??` (MOV RAX,[rip+disp]) e.g. @ `0x6c5b78`, bytes `48 8B 05 41 55 6B 03`, disp `0x036B5541`.
  - `48 83 3D ?? ?? ?? ?? 00` (CMP qword[rip+disp],0) @ `0x72e5d6`, bytes `48 83 3D E2 CA 64 03 00`, disp `0x0364CAE2`.
- **Chain to the iterable container:**
  `reg = *(u64*)(er+0x3d7b0c0); sub = *(u64*)(reg+0x10); map = sub + 0x720;` (a `std::map`/RB-tree).
  - RB-tree header @ `map+0x8`; **iterate** (engine does this per-frame in `FUN_140b32d00`):
    node `+0x00` left, `+0x08` parent, `+0x10` right, `+0x19` nil/color flag, **`+0x20` key (u64)**,
    **`+0x28` value = registered instance ptr** (`+0x30` second payload word).
  - add = `FUN_140b32880`, remove = `FUN_140b32b90`, insert-impl = `FUN_140b32010` (lower_bound on `node+0x20`).
  - So the resident set of all loaded field instances **IS iterable** from a static base — STEP 1 deliverable met.
  - A sibling container `sub+0x730` (enable/disable, `FUN_140b96940/950`) — note, not needed.

### STEP 2 — NEW STRUCTURAL LEAD (supersedes the RPM "no link" verdict above)
The prior RPM link-check probed geom `+0x600`/MsbPart/FieldIns and found no shared key — but it never
probed the **`CS::CSGrowableNodePool<CS::FieldInsBase*>` embedded INSIDE each geom/asset instance at
`+0x3A8`**, which `FUN_1406c5900` builds (callers = the real asset subclass ctors `FUN_1406b9880` /
`FUN_1406db840`). Layout on the instance:
  - `+0x3A8` pool vtable (`CSGrowableNodePool<FieldInsBase*>`), `+0x3B0` heap, `+0x3B8` capacity(=1),
    `+0x3BC` stride(=8, i.e. one `FieldInsBase*` per node), **`+0x3C0` node-array ptr** (null until grown).
  - the pooled child = a `FieldInsBase*` (the asset's loot gimmick); child carries name@+0x00, **lotId@+0x50**.

**Two candidate join paths — both need ONE runtime RPM read to confirm on the known chest
`AEG099_090_9000` / lot `1037500100`:**
- **(A) embedded child-pool (cheapest):** walked `CSWorldGeomStaticIns` → read `+0x3A8` pool → node
  array `+0x3C0` → child FieldIns → `lotId@+0x50`. Validates the asset→lot link with NO global walk.
  *RPM probe:* on the known chest's geom instance, dump `+0x3A8..+0x3D0`; if `+0x3C0` is a heap ptr to
  an array of FieldIns*, follow → expect `lotId@+0x50 == 0x3dd6fec4`.
- **(B) global registry (self-sufficient set):** iterate the STEP-1 RB-tree; each `node+0x28` value is a
  field instance — read its lotId/pos directly, no geom join. Gives every loaded field instance incl.
  ERR-added loot, keyed by the 64-bit id.

### Net
STEP 1 is done (static anchor `er+0x3d7b0c0` + iterable registry chain + node layout). STEP 2 has a
concrete embedded-pool lead at `instance+0x3A8` that the earlier RPM verdict missed; confirm path (A)
with one RPM read, else fall back to iterating the registry (B). Scripts `find_fieldins{,2..6}.java`.

---

## ★ RUNTIME RESULT — path (A) REFUTED as a passive walk (in-process [FIELDINS] probe, 2026-06-23)

In-process probe `diag_fieldins_join` (commit 295fb1a, `goblin_collected.cpp` read_wgm_snapshot): for
every loaded AEG asset, read its embedded `+0x3A8` pool and follow the child.

**Result (24 assets incl. the known chest `AEG099_090_9000`):**
`pool_vt = 0x142a84ca0` (= `CSGrowableNodePool<FieldInsBase*>` RTTI 0x2a84ca0 + imagebase → the
embedded pool IS correctly identified), `cap=1 stride=8` as RE'd — **but `node_arr = 0x0` on ALL of
them.** The pool exists on the asset but its node array was never grown → no child FieldIns → no lotId.

**Verdict:** the loot-gimmick FieldIns is **NOT parented onto the asset instance at tile-load** (the
embedded pool stays empty; it likely grows only when the treasure spawns its content / the chest is
opened). So **path (A) is open-time-only — dead as a passive resident walk.** (The same-session
[LOOTPOS] still reads 100% — geom position is fine; only the asset→lot link is absent here.)

The FieldIns that the prior heap-scan found resident (with `lotId@+0x50 == 0x3dd6fec4`) therefore lives
in the **global** pool/registry, not the per-asset embedded one → **path (B) is the live link.** Next:
one-shot probe iterating the STEP-1 registry `er+0x3d7b0c0 → reg → [+0x10] → +0x720` RB-tree, read each
`node+0x28` instance's vtable + `lotId@+0x50`; confirm a treasure FieldIns with a non-zero lotId is
resident at tile-load (before open). Bulk/throttle/one-shot (Wine RPM).

---

## ★ RUNTIME RESULT — path (B) global-registry walk INCONCLUSIVE in-process (2026-06-23)

One-shot in-process walk of the documented registry chain `er+0x3d7b0c0 → reg → [reg+0x10] →
sub+0x720 → map+0x8 (head) → head+0x08 (root)`, treating it as an MSVC RB-tree (node +0x00 left /
+0x10 right / +0x28 value=instance), reading `lotId@+0x50` per instance. (`diag_fieldins_join`,
commit e63e003, gated in-world.)

**Result, two iterations:**
- WITHOUT a visited-set: ballooned to the 60k node cap, `instances=29436 nonzeroLot=18580` — but the
  sample was the **same instance 24×** (a `MapIns`, vt `0x2a8f650`, `lotId@+0x50 = 0xffffffff` = no-lot
  sentinel). The walk was **revisiting nodes (cycle) / reading garbage as children** — the big counts
  were noise.
- WITH a visited-set + real-lot filter (lot ≠0/≠0xffffffff/≥0x10000000): **`distinctNodes=2,
  instances=2, realLot=1, targetLotHits=0`.** Only ONE real-lot instance reachable (`inst 0x27297880`,
  vt `0x1430308e8` (RVA 0x30308e8 — NOT the `FieldInsBase` 0x2a25e68 family), `lotId 0x3c8ab7cb`),
  not the target chest.

**Verdict:** the chain `sub+0x720` as walked resolves to a ~2-node structure, **not** the
hundreds/thousands-entry field-instance registry the Ghidra pass described — so either an offset
drifted, the container is not a plain `std::map<u64,ptr>` with this node layout, or `head`/`root` sit
elsewhere. **Path (B) is NOT confirmable by in-process RPM with the current offsets.** Combined with
path (A) being empty at tile-load, the runtime asset→lot link for the explore-cache **premium**
(ERR-added loot with names) is **not reachable from the documented structures in-process** → needs a
Ghidra pass to nail the exact container + node layout + the treasure FieldIns vtable, AND to settle
the make-or-break question below.

## ⚠️ The make-or-break question for the whole feature
Path A proved the loot FieldIns is **NOT parented on the asset at tile-load**. So: **does a treasure's
lotId-bearing FieldIns exist ANYWHERE resident BEFORE the chest is opened, or only after?** If
only-after, an explore-cache can only catch loot the player has already OPENED (near-useless — you'd
have to open everything). The prior heap-scan that found `0x3dd6fec4` resident may have been on an
already-opened chest, or may have matched the `ItemLotParam` table copy (the other family it noted),
not a pre-open FieldIns. Ghidra must confirm pre-open residency, else the feature is moot.
