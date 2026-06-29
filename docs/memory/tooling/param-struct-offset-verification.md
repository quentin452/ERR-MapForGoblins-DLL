---
name: param-struct-offset-verification
description: "When hand-writing a from::paramdef struct for the DLL, derive field offsets from the LIVE in-memory row bytes, not the XML paramdef or SoulsFormats — both mis-anchor; pack(1) + dump-and-verify"
metadata: 
  node_type: memory
  type: feedback
---

**UPDATE 2026-06-27 — for single FIELDS, prefer reading the offset LIVE from the exe.** A
"param-offset source-of-truth" mechanism shipped (commits 72c765d, 13a1107, 1ca8e79, 90ee351,
ccdcbbc): an embedded find-what-accesses AOBs the game's OWN field-access instruction and lifts
the offset out of its ModRM displacement at init (self-correcting across patches). Done for
EquipParamGoods.goodsType/sortGroupId, AssetEnvironmentGeometryParam.pickUpItemLotParamId,
isEnableRepick (offset + bit), BonfireWarp textId1 (consensus over multi-match). AOBs live in
`src/re_signatures.hpp` (`*_ACCESS`), use `modutils::resolve_field_offset`. So: hand-derive +
dump-verify (below) is still the method for a FULL struct layout, but for a few specific fields
the live-read is more patch-resilient. See [[live-param-vs-baked-data]].

When adding a `from::paramdef::*_ST.hpp` struct that `get_param<T>` reinterprets (e.g. BonfireWarpParam, 2026-06-23), DO NOT trust offsets computed from the XML paramdef bit-packing rules or from a SoulsFormats `Param.Write()` re-serialize. Both led us astray by ±1 byte three builds in a row:
- XML bit-packing is subtle: `dummy8` bitfields do not always pack the way a naive size-based pass assumes, and a SoulsFormats file re-serialize can carry a leading byte the loaded in-memory row doesn't.
- `static_assert(offsetof(...))` only proves the C++ struct matches YOUR claimed numbers — it cannot catch a wrong claim.

**Why:** ER param rows are loaded into memory as raw byte blobs at the paramdef layout, but the layout is byte-PACKED (fields land on unaligned offsets, e.g. BonfireWarp posX@0x24) — a natural-alignment struct silently misplaces floats. And our offline analysis tooling anchors differently than the live rows.

**How to apply:**
1. Make the struct `#pragma pack(push,1)`; include a partial head only (get_param strides by the param's own row size, so trailing fields can be omitted — see WORLD_MAP_PIECE_PARAM_ST.hpp).
2. Verify offsets by dumping the LIVE in-memory row: a one-shot gated log `reinterpret_cast<const uint8_t*>(&row)` of the first ~6 rows (hex + parsed fields), read a known row's values back. This is GROUND TRUTH; everything else is a hint.
3. Watch packed bitfields: BonfireWarp dispMask00/01/02 are bits 0/1/2 of a SINGLE byte (0x1e), not split across bytes — reading `&0x3` instead of `&0x7` silently dropped every DLC grace.
4. When a "fix" makes a symptom worse, suspect the diagnostic anchor, not just the code — re-derive from raw bytes. (Echoes [[disambiguate-bug-symptoms-first]].)

Decrypt ERR `regulation.bin` on Linux for offline cross-checks via `SoulsFormats.SFUtil.DecryptERRegulation` (AES managed; inner params NOT Oodle-blocked) in a pythonnet venv. See [[err-underground-grace-gate]], [[live-param-vs-baked-data]].
