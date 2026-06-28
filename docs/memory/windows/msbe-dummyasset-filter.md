---
name: msbe-dummyasset-filter
description: "Disk-loot DummyAsset filter — MSBE part-type @+0x0c (13=Asset/9=Dummy), the 3 recover-later reachable_dummy lots, and the 21 false-positive residuals"
metadata: 
  node_type: memory
  type: project
---

Disk-MSB loot (config `loot_from_disk_msb`, see [[handoff-loot-from-real-files]]) over-emitted ~178
markers vs the bake. Root cause: the offline pipeline (`tools/extract_all_items.py:390-516`) excludes
**312 "unreachable_only_lots"** via 4 criteria — Asset GameEditionDisable / overridden-treasure /
**dummy-only-no-Entity** / position-clip-vs-vanilla. The disk walk had no filter.

**RE WIN (validated over all 487 ERR DFLT maps):** MSBE `PartsParam` entry **`+0x0c` = part-type
enum: `13`=Asset, `9`=DummyAsset**. 305/312 unreachable = type 9. Parser reads `Treasure.partType`;
`loot_disk` drops type 9. One field → 97.8% of exclusions, no EntityID offset, no baked list. In-game
disk-only collapsed **178 → 21**. Commits 6f62ed7 (wiring) + b8b2911 (filter).

**✅ EntityID OFFSET PINNED + reachable-dummy recovery DONE (2026-06-24, branch
feat/msbe-entity-recover-dummy, commit a80bf63, UNPUSHED — <user> pushes).** Part **entity sub-struct
ptr @ PART entry +0x60** (entry-rel disk / abs VA resident, eio base); inside: **EntityID @ sub+0x00**,
**EntityGroupIDs[8] @ sub+0x1c..+0x38**. `msbe_parser` now reads `Treasure.entityId`/`entityGroup`;
`loot_disk` KEEPS a type-9 DummyAsset iff `entityId!=0 || entityGroup` (else drops as inert).
Validated over ALL ERR _00 maps (`tools/validate_dummy_entity.py`): exactly **3** type-9 lots are
entity-bound, **0** in `unreachable_msb_lots.json` → **zero new false positives** (178→21 win holds;
305/315 inert still dropped). Pinned via `tools/probe_entity_offset.py` + `probe_group_offset.py`
(SoulsFormats authoritative value × raw-blob match).

**⚠️ RECOVER-LATER now 3 → 1:** **4910 (m12, EntityID 12031490)** + **15000990 (m15, EntityID
15001810)** RECOVERED — now disk-emitted (identical marker/pos as the bake, but survive bake removal).
**2046460000 (m61_46_46 DLC)** is **NOT recoverable via the entity offset**: its DummyAsset has
EntityID 0 + all groups 0 in the MSB *and* is itself in `unreachable_msb_lots.json` (entity-less —
EMEVD lotId-direct / cut). It's the lone residual recover-later lot, still bake-backed. Runtime log:
`[LOOTDISK] RECOVER-LATER inert-dummy lot ...` (gated behind diag_loot_pos now). Recover-later
criterion still = `baked_lot1 ∧ ¬disk_lots ∧ dropped-as-inert`.

**21 false positives still shown (benign, 0.6%, no missing loot):** ~7 position-clip (need vanilla,
e.g. Golden Seeds m60_42_51/43_52), ~10 type-**10** crafting subtype (m21_02 AEG463_610, goods
2020001 — filterable via the same +0x0c), ~4 extracted-but-unlinked.

Full detail: `docs/re/windows_msbe_dummyasset_unreachable_re_findings.md`. Part type-10 = a third
subtype seen on crafting gather assets.
