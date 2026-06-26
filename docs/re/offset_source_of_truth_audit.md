# Audit — offset source-of-truth (no-bake Phase 3 / "industrial offset-free")

Goal of the refactor: stop hand-pinning byte offsets. Make them derive from a **source of truth**
(the game's paramdefs for params; SoulsFormats format for disk files), so a game patch that shifts a
field can't silently break loot/classification, and the DLL works for any mod with no per-item data.

## Key finding — item LOADING is already generic

Every item-source mechanism resolves the item via a **live param chain**, never a per-item table:

| Mechanism | Code | Item resolution (all LIVE) |
|---|---|---|
| Disk MSB Treasure | msbe_parser + loot_disk | EVENT.itemLotId → ItemLotParam_map |
| Disk MSB Asset (AEG) | loot_disk collectibles | AEG.pickUpItemLotParamId(+0xb8) → ItemLotParam_map |
| Disk MSB Enemy | loot_disk enemy drops | NpcParam.itemLotId_map/_enemy → ItemLotParam |
| EMEVD awards / boss / per-tile | parse_emevd(_full) | template (entity,lot) → ItemLotParam |
| EMEVD gestures | parse_emevd_gestures | GestureParam.itemId → goods |
| Graces | inject_graces | BonfireWarpParam (live) |
| Static bake | goblin_map_data | category enum (being retired) |

→ "load all files for all items" is **already the architecture**. The remaining work is NOT a loader
rewrite; it is removing the ~30 hand-pinned offsets these chains depend on.

## The hand-pinned offsets (the source-of-truth gap)

### Bucket 1 — PARAM field offsets (the real win; src/goblin_inject.cpp)
| Param | Field | Offset | How pinned today |
|---|---|---|---|
| EquipParamGoods | goodsType | +0x3e (u8) | empirical raw-row dump |
| EquipParamGoods | sortGroupId | +0x72 (u8) | paramdef field-layout walk |
| AssetEnvironmentGeometryParam | pickUpItemLotParamId | +0xb8 (s32) | vs paramdef DetectedSize=320 |
| AssetEnvironmentGeometryParam | isEnableRepick | +0x3c bit5 | empirical (was the bit5/6 16k leak) |
| NpcParam | itemLotId_enemy / _map | +0x30 / +0x34 | vs paramdef (probe_enemy_npc_offset.py 26277/26277) |
| NpcParam | nameId / teamType | +0x0c / +0x133 | empirical |
| BonfireWarpParam | eventflagId/pos/iconId/textId | hand struct | in-memory [BONFIRE-PROBE] dump |
| GestureParam | itemId | (field) | live read |
| ItemLotParam | whole row | 0x98 opaque | size-verified; fields walked at runtime |

These are version-fragile. **Source of truth = the paramdef** (the same XML that auto-generated
`WORLD_MAP_POINT_PARAM_ST.hpp` / the other 4 generated headers — generator NOT in this repo).

### Bucket 2 — disk MSB/EMEVD structural offsets (lower risk; src/worldmap/msbe_parser.cpp)
part type +0x0c / pos +0x20 / entity +0x60 / enemy typeData +0x68; region subtype +0x08 / pos +0x14;
treasure typeData partIndex +0x08 / itemLotId +0x10; EMEVD header +0x10/+0x18/+0x28/+0x78; GameEdition
+0x44. All pinned vs SoulsFormats (probe_*.py, 500/500, 6612/6612, 26277/26277). **The MSB/EMEVD format
is stable across game patches** → these drift far less than param offsets; lower priority.

## paramdef header state (src/from/paramdef/, 5 headers)
- `WORLD_MAP_POINT_PARAM_ST` / `WORLD_MAP_PIECE_PARAM_ST` / `BONFIRE_WARP_SUB_CATEGORY_PARAM_ST` /
  `WORLD_MAP_LEGACY_CONV_PARAM_ST` — **auto-generated from XML paramdefs** (full structs; one sizeof
  static_assert). Generator lives OUTSIDE this repo.
- `BONFIRE_WARP_PARAM_ST` — **hand-written** partial (#pragma pack(1) + per-field offsetof static_asserts).
- The params the loot/classify path reads (EquipParamGoods, ItemLotParam, NpcParam,
  AssetEnvironmentGeometryParam, GestureParam) have **NO generated header** → they use raw byte arrays +
  hand-pinned offsets in goblin_inject.cpp. **This is the gap to close.**

## Plan (proposed)
- **Phase 2a — paramdef-driven param structs (the win).** Obtain/port the paramdef→C++ generator (the
  one behind the 4 generated headers) + the paramdef defs (SoulsFormats Paramdex) for the loot/classify
  params. Generate full structs for EquipParamGoods, ItemLotParam, NpcParam, AssetEnvironmentGeometryParam,
  GestureParam. Replace the raw-row + hand-offset reads in goblin_inject.cpp with generated field access
  + keep a sizeof static_assert. Result: no hand-pinned param offset survives.
- **Phase 2b — disk format (optional/later).** MSB/EMEVD offsets are format-stable; either leave pinned
  (with the existing probe_*.py regression guards) or generate from a SoulsFormats format spec.
- **Then Phase 3 (delete the bake)** becomes safe: every item is sourced live by a paramdef-true chain.

## Open dependency (blocks Phase 2a)
WHERE is the source of truth obtained? (a) the external generator + Paramdex the 4 headers came from,
(b) a SoulsFormats Paramdex checkout, or (c) parse the paramdef from the loaded regulation at runtime.
This choice determines the implementation path.

## PROTOTYPE (2026-06-26) — runtime offset-read mechanism PROVEN live (`D:\ghidra_scripts\offset_resolver.py`)

quentin's clarified goal: don't extract-and-hardcode; have the DLL READ the offset from the game's own
access instruction at init (`movzx eax,byte [row+0x3e]` carries 0x3e as its ModRM displacement). AOB the
read site → extract the displacement → self-correcting, zero offset constants.

Built + ran the prototype against the live game (2.6.2.0). Three parts, all green:
- **Part A — displacement decoder.** `decode_disp()` parses prefixes→REX→1/2-byte opcode→ModRM→[SIB]→
  disp8/disp32 and returns `(offset, disp_pos, disp_size)`. Self-test 6/6 (write/read forms, disp8/disp32,
  negative disp8). The runtime extraction itself is **trivial** — it is a sibling of the DLL's existing
  `modutils` `relative_offsets` (read N bytes at a fixed pattern position), just RETURN the value instead
  of using it as a pointer delta. No disassembler needed at runtime; the authored AOB pins the disp position.
- **Part B — live param oracle.** Ported `from::params::get_param` to RPM: `SOLO_PARAM_LIST` AOB → param_list
  → `EquipParamGoods` table (7126 rows) → any row's bytes. e.g. goods 8000 `goodsType[+0x3e]=1` (key item),
  goods 1000 `sortGroupId[+0x72]=10`. This is the ground-truth value any resolved offset must explain.
- **Part C — author→resolve round-trip.** `author_aob_from_rip()` turns a captured read-site RIP into a
  uniqueness-checked AOB with the disp wildcarded; `resolve_offset()` is the exact runtime path (scan +
  read disp). Proven end-to-end on a real exe instruction (the SOLO_PARAM_LIST site): authored a UNIQUE
  24-byte AOB, runtime-resolved its displacement = live value (`0x3053605`, round-trip OK). The uniqueness
  gate also fires correctly (an 11-byte AOB → 339 matches → flagged NOT UNIQUE).

**Net:** the engine + oracle are done and proven. The ONLY remaining per-field cost is **capturing the
read site** — a blind 0x3e scan is noise (`find_goods_access` → 4 hits, all WRITES/ctors), so each field
needs a param-anchored capture. The fast loop on this box = CE **"find what accesses"** on a LIVE row
address (Part B prints the exact `rowAddr+fieldOff` target). Then `offset_resolver.py author 0x<RIP>`
emits the AOB. Handoff CLI: `author` / `resolve` / `groundtruth`.

**Value vs. the shipped Paramdex guard:** both turn silent drift into a loud failure. The runtime-read's
unique advantage is the COMMON drift case — a field inserted earlier shifts later offsets but leaves the
read instruction's encoding identical except the disp; the AOB still matches and resolves the NEW offset
with **zero human action**. The Paramdex guard would instead fail the build and need a manual two-place
update. So runtime-read genuinely reduces maintenance — at the cost of ~6 one-time read-site captures.

## RESULT (2026-06-26) — the source of truth is LIVE bytes, not the Paramdex

Tried the cheapest authoritative route first: read the offset from the exe's compiled code (AOB the
access instruction → its displacement IS the offset). Probe (`find_goods_access.java`): scanning for a
displacement (even filtered to memory operands + the goodsType+sortGroupId co-access heuristic) does
NOT isolate the read site — it surfaces generic struct serializers. Locating a field's access site
needs param-anchored RE per field (string → SoloParamRepository → getter). **Verdict: exe-AOB is
mechanically trivial but EXPENSIVE per field.**

Then found the **SoulsFormats Paramdex is already committed** at `tools/paramdefs/*.xml` (~200 defs incl.
every param we read) + `D:\ghidra_scripts\paramoff.py` computes field offsets from it. Built
`tools/check_param_offsets.py` to cross-check our hardcoded offsets against the Paramdex (fixed a
bitfield-packing bug: pack by storage SIZE, not type name — `u8 x:1`+`dummy8 y:7` share one byte).

Result: **7/11 offsets confirmed; 4 legitimately DRIFT** (Paramdex ≠ ERR regulation):
`sortGroupId` (pd 0x73 vs code 0x72), `pickUpItemLotParamId` (pd 0xb9 vs 0xb8), `textId1` (pd 0x31 vs
0x30), and **`isEnableRepick` (pd bit6 vs code bit5)** — the latter is the historical 16k-leak field:
the Paramdex's bit6 is exactly what the original (buggy) code used; the fix to bit5 came from LIVE
param bytes. So the committed Paramdex is a version-drifted snapshot, NOT byte-exact to ERR 2.6.2.0.

**Conclusion:** the authoritative source of truth is the **live param bytes** (pinned via raw bytes +
POSITIVE/NEGATIVE samples — memory `re-offset-validation`), already done historically; the code's
offsets are correct. The Paramdex is a useful **advisory cross-check** (catches gross drift) but cannot
be the sole source — it drifts. `tools/check_param_offsets.py` is committed as that advisory guard
(soft: flags expected drift, HARD-fails only on a new/unexpected divergence). A fully-automated HARD
guard would need a live-RPM SoloParamRepository row reader (the user's proven method, not yet scripted
as a standalone tool). Net: **no offset code change needed** (they're right); Phase-2a's value is the
guard, not a Paramdex-codegen rewrite (which drift makes unreliable anyway).
