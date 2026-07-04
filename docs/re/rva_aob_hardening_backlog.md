# AOB-hardening backlog — fixed-RVA resolutions that need byte signatures

**Re-verified current 2026-07-04** (full re-scan of `_RVA` symbols + `er+0x` literals across `src/`):
every fixed-RVA site in code is already listed below. No new resolutions since the initial inventory —
the only delta was `RENDER_MGR_SLOT_RVA` (a local alias of `CANVAS_SINGLETON_RVA` 0x47ef360, now noted).
All other uncovered `er+0x` hits are comments / log strings restating documented RVAs (WorldChrMan
fallback already dual-pathed; FieldIns `er+0x3d7b0c0` is comment-only — impl uses the WGM walk). The
A7 region-labels work added zero RVAs. **Update 2026-07-04: the 3 hot-path FUNCs + the cheap-win
switches are now DONE (see the ✅ sections); remaining = PHYSWORLD slot, the 2 hooked grace fns, and
the vtable/slot + icon-harvest sets.**

Inventory 2026-07-04. RVAs are **build-specific** (ERR 2.2.9.6): they break on a game patch and are not
mod-agnostic. Doctrine (`re_signatures.hpp`) is to pin AOBs, keep an RVA only as a cross-checked
fallback. This lists every fixed-RVA / hardcoded module-relative resolution still lacking an AOB, so it
can be hardened. STRUCT FIELD offsets (`+0x98`, `+0x6C0`, `+0x1E508`, …) are stable and excluded.

**See also [fragile_primitives_audit.md](fragile_primitives_audit.md)** — ranks the primitives that are
MORE dangerous than a fixed RVA (engine vtable slot indices, code-patching hooks, direct engine/save
writes) plus the field-offset silent-failure surface and the QPC verdict (leave it).

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
| WARPPIN_BUILDER_FN | 0x88b7b0 | WarpPinData builder (**hooked**) | goblin_grace_suppression.cpp |
| WARPPIN_SETTO_FN | 0x87ae20 | vt[1] SetTo (**hooked**) | goblin_grace_suppression.cpp |

The 2 grace fns (tier-S: RVA + code-patch write + crash-on-wrong, the audit's top items) were the
trickiest — being HOOKED, a live dump reads the detour JMP, not the prologue. **Captured by booting with
`grace_suppress_native=false`** (the hook install is fully gated on that flag → the RVAs hold their
original prologue), dumping via the normal `mem_dump`, then restoring the ini. Both verified PASS+UNIQUE
+ hooks install at the AOB-resolved address (init order is safe: `[SIG]`/resolve run BEFORE the hook
patches the bytes). Trampoline-read was NOT needed — the flag-gate is simpler.

## FUNC — call targets with NO AOB (move every patch)
| symbol | RVA | resolves | file |
|---|---|---|---|
| icon-harvest | er+0xd63c30,0x1f5560,0xd77550,0xd771d0,0xd69640,0xd724c0 | image-repo find/load/group/tick | goblin_icon_harvest.cpp |

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

## ✅ Cheap wins — DONE 2026-07-04 (switched call site to the existing AOB)
- **WorldChrMan** — `WCM_FINDER` AOB + `er+0x3D65F88` fallback (already dual-pathed; fine — no change).
- **CSMenuMan** — ✅ `worldmap_probe.cpp` now resolves the slot via a cached `csmenuman_slot(base)`
  (`CSMENUMAN_SLOT` AOB + `relative_offsets{{3,7}}`, RVA fallback) at all 6 read sites. `[SIG]` PASS live.
- **CreateImage** — ✅ the hook install AOB-address is cached in `g_create_image_addr` and reused by the
  two force paths (RVA fallback kept). Live-verified: the AOB resolved to EXACTLY `er+0xd6bbc0` and the
  hook installed there, so the force calls hit the identical function.

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
~~Hot-path FUNCs (CASTRAY, ENSURE_ASSET_REQUEST, GETTER)~~ ✅ + ~~cheap-win switches (CSMenuMan,
CreateImage)~~ ✅ + ~~the 2 hooked grace-suppression FUNCs (tier-S, the audit's top danger)~~ ✅ DONE
2026-07-04. Remaining, in order: **PHYSWORLD** slot (heightfield — FWA a load site); the tier-S engine
**vtable slots** (geom setter 26, grace SetTo vt[1] — resolve/assert from a ctor AOB, per
`fragile_primitives_audit.md`); then the worldmap vtables/slots and the icon-harvest set.

Note: `goblin_markers.cpp` old RVAs 0x3D5DF38/0x2AC21D8 are already migrated to AOB (comments only).
See [re-signatures-registry](../memory/tooling/re-signatures-registry.md) for how to add a signature.
