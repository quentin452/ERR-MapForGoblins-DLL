# AOB-hardening backlog — fixed-RVA resolutions that need byte signatures

> Detecting + recovering from a patch that breaks these lives in
> [`patch_diff_maintenance.md`](patch_diff_maintenance.md) (build fingerprint + binary-diff recipe).

Inventory 2026-07-04. RVAs are **build-specific** (ERR 2.2.9.6): they break on a game patch and are not
mod-agnostic. Doctrine (`re_signatures.hpp`) is to pin AOBs, keep an RVA only as a cross-checked
fallback. This lists every fixed-RVA / hardcoded module-relative resolution still lacking an AOB, so it
can be hardened. STRUCT FIELD offsets (`+0x98`, `+0x6C0`, `+0x1E508`, …) are stable and excluded.

## FUNC — call targets with NO AOB (move every patch)
| symbol | RVA | resolves | file |
|---|---|---|---|
| ENSURE_ASSET_REQUEST_RVA | 0x6a5080 | EnsureAssetRequest(reqMgr,name) | goblin_geom_spawn.cpp:19/100 |
| GETTER_RVA | 0x6c46e0 | geom matrix getter (6 sites) | goblin_geom_move.cpp:21 |
| CASTRAY_RVA | 0xc70360 | PhysWorld cast-ray (heightfield) | goblin_heightfield.cpp:22 |
| — | er+0x88b7b0 | WarpPinData builder (**hooked**) | goblin_grace_suppression.cpp:126 |
| — | er+0x87ae20 | vt[1] SetTo (**hooked**) | goblin_grace_suppression.cpp:148 |
| icon-harvest | er+0xd63c30,0x1f5560,0xd77550,0xd771d0,0xd69640,0xd724c0 | image-repo find/load/group/tick | goblin_icon_harvest.cpp |

## STATIC-SLOT — singleton/vtable slots with NO AOB
| symbol | RVA | resolves | file |
|---|---|---|---|
| PHYSWORLD_RVA | 0x3d76060 | CS::PhysWorld FD4Singleton (heightfield) | goblin_heightfield.cpp:23 |
| WORLDMAP_VIEWMODEL_VTABLE_RVA | 0x2ad82e0 | WorldMapViewModel vtable (+page table) | re_signatures.hpp:272 |
| CURSOR_VTABLE_RVA | 0x2b29a90 | WorldMapCursorControl vtable (many sites) | re_signatures.hpp:305 |
| ICON_MGR_SLOT_RVA / _SIBLING | 0x3d6e9b0 / 0x3d6f558 | CSWorldMapPointMan (native-pin suppression) | re_signatures.hpp:315/316 |
| CANVAS_SINGLETON_RVA | 0x47ef360 | WorldMap canvas/render mgr | re_signatures.hpp:309 |
| — | er+0x2ad8228 | WorldMapWarpPinData vftable | goblin_grace_suppression.cpp:147 |
| icon-harvest | er+0x3d82510, er+0x3d5b0f8; base+0x2f05928, +0x2b761b0 | image-repo/CSFile slots + GX vtables | goblin_icon_harvest.cpp |

## Cheap wins — AOB already exists, but an RVA is still used (just switch the call site)
- **WorldChrMan** — `WCM_FINDER` AOB + `er+0x3D65F88` fallback (already dual-pathed; fine).
- **CSMenuMan** — `CSMENUMAN_SLOT` AOB exists, but `worldmap_probe.cpp` uses `CSMENUMAN_SLOT_RVA` 0x3d6b7b0 everywhere → switch to the AOB.
- **CreateImage** — `WORLDMAP_CREATE_IMAGE` AOB exists, but `icon_harvest.cpp:1352/1370` uses `er+0xd6bbc0` → switch.

## Diag/debug-only (behind config flags — lowest priority)
`goblin_collected.cpp` (diagFieldinsJoin/diagLotMemscan slots + vtables), `goblin_geom_move.cpp:412-414`
(3 diag-dump globals), `worldmap_probe.cpp` area/layer/tile vtables — all gated, harden last.

## How to harden — ALL doable on Linux (RVAs are known + module loads ~1:1)
An AOB matches RUNTIME bytes, so packed/VMProtect'd code is fine (mem_dump gives the real bytes; scan
matches them). Windows Ghidra is only needed to DISCOVER a new function (not the case — all known) or if
a region is VMProtect-*mutated* per-run (no stable AOB exists — rare; detect by an incoherent prologue).
- **FUNC** (call target): `mem_dump` ~48-64 bytes at `er+RVA` → craft an AOB from the prologue
  (wildcard rip-rel disps/immediates; include enough bytes to be unique). FWA not needed. Dumping also
  verifies the RVA is right (sane prologue) vs a packer mismatch.
- **STATIC-SLOT** (singleton/data ptr): arm **find-what-accesses** on `er+RVA` (the slot) → capture the
  RIP of a `mov reg,[rip+disp]` that reads it → `mem_dump` at that RIP → AOB + `relative_offsets`.
- **vtable**: FWA on the vtable address, TRIGGER the object's construction/use (open map / spawn) →
  capture the `lea [rip+disp]` RIP → AOB.
- **Cheap wins** need no RE — the AOB already exists, just switch the call site.

## Priority
Hot-path first: CASTRAY + PHYSWORLD (heightfield, brand-new), ENSURE_ASSET_REQUEST + GETTER (geom),
the two **hooked** grace-suppression FUNCs (a wrong hook address = crash). Then the worldmap vtables/
slots. The three cheap-win switch-to-existing-AOB items are near-free and should go first of all.

Note: `goblin_markers.cpp` old RVAs 0x3D5DF38/0x2AC21D8 are already migrated to AOB (comments only).
See [re-signatures-registry](../memory/tooling/re-signatures-registry.md) for how to add a signature.
