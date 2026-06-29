---
name: err-underground-grace-gate
description: "ERR's real underground/cave grace gate = BonfireWarpParam.iconId per grace (1=normal, 44=underground), NOT areaNo — found by decrypting ERR regulation on Linux"
metadata: 
  node_type: memory
  type: reference
---

How ERR decides a Site of Grace draws its cave/underground map icon vs the vanilla bonfire icon — the REAL gate (RE'd 2026-06-23 by decrypting ERR `regulation.bin`).

**Grace location-anchors ALSO live now (commit 4a8022a, 2026-06-23).** The grace-anchor table (region
PlaceName label + map sub-page tabId + cluster anchoring) was the last baked grace data — derived live
instead: `LiveGrace` gained `bonfireSubCategoryId`; `goblin::grace_anchors()` lazy-builds from
`live_graces()` + new partial paramdef `BONFIRE_WARP_SUB_CATEGORY_PARAM_ST` (tabId @0x08) for
subCat→region-PlaceName(=subCat when it resolves)+tabId. Deleted `generated/goblin_grace_anchors.*` +
`tools/build_grace_anchors.py` (−472 lines). Validated in-process vs the old bake first: tabId offset 0
mismatch, all 15 deltas = stale bake (6 ERR dispMask-hidden spoilers the bake wrongly kept, 5 stale
subCat, 4 stale m11 grid). `grace_position_index.json`/`build_grace_index.py` KEPT (offline per-marker
location-name resolution). Graces now FULLY live: names + iconId-gate + positions + anchors.

**Gate = a per-grace field in `BonfireWarpParam`** (NOT a dungeon-type / areaNo rule):
- `iconId = 1`  → normal grace (MENU_MAP_01_Bonfire) — 350 graces
- `iconId = 44` → underground/cave grace (→ MENU_MAP_44 cell, which ERR routes to its GraceUnderground sprite) — 93 graces
- `forbiddenIconId` paired: 2 = normal-undiscovered, 64 = underground-undiscovered (confirms the couple). iconId 48 = 1 unique grace.
- ERR hand-set iconId=44 per grace; it is authored, not derivable from area alone.

**Which graces get 44 (the 93):** ALL catacombs (m30, 18) + ALL caves/grottos (m31, 20) + most tunnels (m32, 4) + scattered small-dungeon graces (m13/16/18/22/25/35/39/40/41/42/43) + **16 overworld graces** (m60:13, m61:3) that sit physically below the surface (wells/sub-surface, posY −96…+1626) = CHANGELOG L5839 "dungeon graces and graces below the visible map".

**Which stay iconId=1 (decisive counter-examples):** **m12** (Siofra/Ainsel/Deeproot/Mohgwyn — the big underground region, 34 graces) → all NORMAL, because it has its OWN map layer. **m10/m11** (Stormveil, Raya Lucaria) + m13/14/15/20/21/34 legacy dungeons → normal (own map layer). ⇒ rule = small dungeons WITHOUT their own map layer + below-surface overworld graces get the cave icon; anything with a dedicated map layer keeps the bonfire icon.

**IMPLEMENTED (2026-06-23):** the DLL now reads graces LIVE from `BonfireWarpParam` and gates underground on `iconId==44`. Changes: new `src/from/paramdef/BONFIRE_WARP_PARAM_ST.hpp` (packed partial struct, posX at unaligned 0x25 → `#pragma pack(1)`, offset static_asserts); `capture_live_graces` (goblin_inject.cpp) rewritten to read BonfireWarpParam (filters: areaNo!=0, eventflagId>0, textId1>0, dispMask!=0; discoverFlag=eventflagId, underground=iconId==44) — dropped the old WMP iconId==370 path + MAP_ENTRIES fallback; `LiveGrace` gained `bool underground`; `grace_layer.cpp` `m.dungeon = e.underground` (was the wrong `areaNo==31`). Builds clean (ninja, build-linux). The old baked grace path DELETED: `tools/generate_graces.py`, its build_pipeline stage, both `World - Graces.MASSEDIT` files, list entries in generate_all_massedit/compare_massedit. NB the 427 orphaned `WorldGraces` rows in generated `goblin_map_data.cpp` are now unread (overlay builds no MapEntryLayer for graces — `goblin_overlay.cpp:1220`) and will vanish on next pipeline regen. VERIFIED in-game 2026-06-23 on overworld + underground + DLC maps (commit `cbbec2e` on docs/worldmap-projection-re-brief); also fixed some pre-existing grace anomalies. Debugging took several builds — see [[param-struct-offset-verification]] for the offset-RE lesson. See [[native-icon-render-pipeline]], [[overlay-rendered-markers]].

**Struct layout (IN-MEMORY, the only one that matters — `BONFIRE_WARP_PARAM_ST.hpp`, `#pragma pack(1)`):** eventflagId@0x04, bonfireEntityId@0x08, forbiddenIconId@0x10, bonfireSubCategoryId@0x14, clearedEventFlagId@0x18, iconId@0x1c, **dispMask@0x1e** (dispMask00/01/02 = bits 0/1/2 of the SINGLE byte 0x1e; 0x1f is pad), areaNo@0x20, gridXNo@0x21, gridZNo@0x22, posX@0x24, posY@0x28, posZ@0x2c, textId1@0x30 (head size 0x34). HARD-WON: don't trust the XML bitfield-packing or a SoulsFormats file re-serialize to derive offsets — both mis-anchored us by ±1. Verify by dumping the live in-memory row bytes (`reinterpret_cast<uint8_t*>(&row)`). Visible/shown filter = `(dispMask0 & 0x7) != 0` (all-zero = ERR spoiler-hide, ~6-8 graces); using `&0x3` wrongly hid every DLC grace (dispMask02=bit2). Grace marker GROUP gates rendering: overworld=0, underground(m12)=1, DLC=2 — only the open map page's group draws.

**Tooling note:** ERR `regulation.bin` decrypt WORKS on Linux via `SoulsFormats.SFUtil.DecryptERRegulation` (AES is managed; the inner params are NOT Oodle-blocked, unlike the menu `.tpf.dcx`/`.sblytbnd.dcx` KRAK sheets). Needs a pythonnet venv (`tools/.venv`) + `tools/lib/Andre.SoulsFormats.dll`. BonfireWarpParam = 444 rows, 79 fields (has iconId/altIconId/forbiddenIconId/altForbiddenIconId).
