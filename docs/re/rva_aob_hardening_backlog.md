# AOB-hardening backlog — fixed-RVA resolutions that need byte signatures

**Re-verified current 2026-07-04** (full re-scan of `_RVA` symbols + `er+0x` literals across `src/`):
every fixed-RVA site in code is already listed below. No new resolutions since the initial inventory —
the only delta was `RENDER_MGR_SLOT_RVA` (a local alias of `CANVAS_SINGLETON_RVA` 0x47ef360, now noted).
All other uncovered `er+0x` hits are comments / log strings restating documented RVAs (WorldChrMan
fallback already dual-pathed; FieldIns `er+0x3d7b0c0` is comment-only — impl uses the WGM walk). The
A7 region-labels work added zero RVAs. The 3 cheap-win switch-to-existing-AOB items are still open.

Inventory 2026-07-04. RVAs are **build-specific** (ERR 2.2.9.6): they break on a game patch and are not
mod-agnostic. Doctrine (`re_signatures.hpp`) is to pin AOBs, keep an RVA only as a cross-checked
fallback. This lists every fixed-RVA / hardcoded module-relative resolution still lacking an AOB, so it
can be hardened. STRUCT FIELD offsets (`+0x98`, `+0x6C0`, `+0x1E508`, …) are stable and excluded.

## ✅ FUNC — hardened 2026-07-04 (AOB-first, RVA cross-check fallback)
Prologue AOBs crafted from a LIVE dump via `tools/hf_hook_scout.py disasm --aob`, added to
`re_signatures.hpp`, and resolved through the new `sig::resolve_func_aob(aob, base, rva, name)` helper
(AOB first; falls back to `base+rva` if the AOB misses; warns on AOB≠RVA disagreement). All three
verified **PASS + UNIQUE** in the live `[SIG]` health check + exercised in-world (2 boots).
| symbol (AOB const) | RVA | resolves | file |
|---|---|---|---|
| ENSURE_ASSET_REQUEST_FN | 0x6a5080 | EnsureAssetRequest(reqMgr,name) | goblin_geom_spawn.cpp |
| GEOM_MATRIX_GETTER_FN | 0x6c46e0 | geom matrix getter (6 sites) | goblin_geom_move.cpp |
| CASTRAY_FN | 0xc70360 | PhysWorld cast-ray (heightfield) | goblin_heightfield.cpp |

## FUNC — call targets with NO AOB (move every patch)
| symbol | RVA | resolves | file |
|---|---|---|---|
| — | er+0x88b7b0 | WarpPinData builder (**hooked**) | goblin_grace_suppression.cpp:126 |
| — | er+0x87ae20 | vt[1] SetTo (**hooked**) | goblin_grace_suppression.cpp:148 |
| icon-harvest | er+0xd63c30,0x1f5560,0xd77550,0xd771d0,0xd69640,0xd724c0 | image-repo find/load/group/tick | goblin_icon_harvest.cpp |

**⚠ The 2 grace fns are already HOOKED** by our DLL — a live dump at their address reads the detour
JMP, not the original prologue, so `hf_hook_scout disasm --aob` can't craft their AOB from a running
game. Harden them by reading the ORIGINAL bytes from the MinHook trampoline (or dump before the hook
installs), then wildcard as usual.

## STATIC-SLOT — singleton/vtable slots with NO AOB
| symbol | RVA | resolves | file |
|---|---|---|---|
| PHYSWORLD_RVA | 0x3d76060 | CS::PhysWorld FD4Singleton (heightfield) | goblin_heightfield.cpp:23 |
| WORLDMAP_VIEWMODEL_VTABLE_RVA | 0x2ad82e0 | WorldMapViewModel vtable (+page table) | re_signatures.hpp:272 |
| CURSOR_VTABLE_RVA | 0x2b29a90 | WorldMapCursorControl vtable (many sites) | re_signatures.hpp:305 |
| ICON_MGR_SLOT_RVA / _SIBLING | 0x3d6e9b0 / 0x3d6f558 | CSWorldMapPointMan (native-pin suppression) | re_signatures.hpp:315/316 |
| CANVAS_SINGLETON_RVA | 0x47ef360 | WorldMap canvas/render mgr | re_signatures.hpp:309 (+ alias `RENDER_MGR_SLOT_RVA` worldmap_probe.cpp:1944) |
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
- **FUNC** (call target): **now one command** — `tools/hf_hook_scout.py disasm er+0x<rva> --len 96 --aob`
  dumps the live prologue (via `er_base`+`mem_dump`), disassembles it, and prints a ready AOB with
  rip-rel disps + rel32 branch targets wildcarded. Paste into `re_signatures.hpp`, add to the
  `all_signatures` table, and resolve at the call site via `sig::resolve_func_aob(aob, base, rva, name)`
  (AOB-first, RVA fallback). Then boot → confirm the `[SIG]` line is **PASS** (not MULTI — extend the
  window with a bigger `--len`/`max_bytes` if MULTI, as GEOM_MATRIX_GETTER needed). FWA not needed.
- **STATIC-SLOT** (singleton/data ptr): arm **find-what-accesses** on `er+RVA` (the slot) → capture the
  RIP of a `mov reg,[rip+disp]` that reads it → `mem_dump` at that RIP → AOB + `relative_offsets`.
- **vtable**: FWA on the vtable address, TRIGGER the object's construction/use (open map / spawn) →
  capture the `lea [rip+disp]` RIP → AOB.
- **Cheap wins** need no RE — the AOB already exists, just switch the call site.

## Priority
~~Hot-path first: CASTRAY, ENSURE_ASSET_REQUEST, GETTER~~ ✅ DONE 2026-07-04. Remaining, in order:
the three cheap-win switch-to-existing-AOB items (near-free); **PHYSWORLD** slot (heightfield — FWA a
load site); the two **hooked** grace-suppression FUNCs (wrong hook addr = crash; read the trampoline,
not the live bytes); then the worldmap vtables/slots and the icon-harvest set.

Note: `goblin_markers.cpp` old RVAs 0x3D5DF38/0x2AC21D8 are already migrated to AOB (comments only).
See [re-signatures-registry](../memory/tooling/re-signatures-registry.md) for how to add a signature.
