# Fragile-primitive audit — what's MORE dangerous than a fixed RVA

Companion to [rva_aob_hardening_backlog.md](rva_aob_hardening_backlog.md). That doc hardens fixed RVAs.
This one answers: **are there primitives more dangerous than an RVA, and where are they?** Audit
2026-07-04 (full `src/` grep).

## Why an RVA is actually one of the SAFER fragile primitives
A fixed RVA (`er + 0x…`) is patch-fragile, but it is:
- **detected** — the `[SIG]` health check + `resolve_func_aob`'s AOB cross-check flag drift at boot;
- **loud** — a wrong function address usually crashes on the first call (or the guard returns null), not
  silently returns wrong data;
- **a read of an address**, not a write.

Rank danger by: *does it fail SILENTLY?* + *does it WRITE?* + *how hard does it crash?* — minus
*is it detected?* By that measure several primitives beat RVA.

## Tier S — more dangerous than RVA (write / arbitrary-call / undetected)

1. **Engine vtable SLOT INDICES.** Calling `(*vt)[N]` with a hardcoded N. Worse than an RVA: no health
   check, and a patch that reorders the vtable calls the WRONG virtual function with a MISMATCHED
   signature → crash or silent corruption. An RVA at least names one function; a stale slot calls
   whatever now sits there.
   - `goblin_geom_move.cpp:23` `SETTER_VSLOT = 26` (`vtable[0xd0]` SetWorldMatrix) — ENGINE vtable, moves on patch.
   - `goblin_grace_suppression.cpp:148` `vt[1]` SetTo — ENGINE vtable, and it's HOOKED (see tier S-2).
   - (SAFE, do not touch: DXGI `vtable[8/13]`, D3D12 `vtable[10]`, DInput `vtable[9/10]` in
     `goblin_overlay.cpp`/`input_directinput.cpp` — those are Microsoft COM ABIs, contractually stable.)
   - **Mitigation:** resolve the slot from the class's ctor AOB (the `mov [rax+N*8], lea vtable` write) or
     assert the vtable base matches a known AOB before the vcall, so a reorder fails LOUD not wrong.

2. **MinHook detour installs (code patching).** ~24 sites in 12 files (`modutils::hook`). A hook WRITES a
   JMP over the target prologue — a wrong address (RVA/AOB drift) or a shorter-than-expected prologue
   corrupts LIVE code → instant crash or subtle corruption. Strictly worse than a read-RVA (it writes).
   - ~~**The two most dangerous unhardened items:** the grace-suppression hooks at `er+0x88b7b0` /
     `er+0x87ae20`~~ ✅ **HARDENED 2026-07-04** (`WARPPIN_BUILDER_FN`/`WARPPIN_SETTO_FN`, AOB-first via
     `resolve_func_aob`). The live-dump-reads-the-JMP problem was solved by booting with
     `grace_suppress_native=false` (the hook install is gated on it) to dump the original prologue, NOT
     the trampoline. Both PASS+UNIQUE; hooks install at the AOB address ([SIG]/resolve run before the
     patch). This retires tier S-2's top items.
   - Everything else routes through an AOB (`icon_harvest`, `overlay`, DXGI/DInput) — lower risk.

3. **Direct engine/​save memory WRITES.** ~17 sites (`write_dw`/`write_bytes`/`WriteProcessMemory`):
   `goblin_inventory.cpp` (6 — the sidecar strip zeroing inventory nodes), `goblin_worldmap_probe.cpp`
   (5), `goblin_param_edit.cpp` (2), `goblin_kindling.cpp` (2), `goblin_collected.cpp` (1),
   `goblin_icon_harvest.cpp` (1). A wrong field offset here WRITES garbage into a live engine struct or
   the on-disk save → save corruption / crash, silently. The inventory strip is the scariest (it edits
   what gets serialized). **Mitigation:** a layout canary (assert a neighbouring known field before the
   write); the strip already snapshots+restores 0x18 bytes, which bounds the blast radius.
   - ✅ **Inventory chain canary DONE 2026-07-04** (`goblin_inventory.cpp` `verify_inventory_layout`):
     asserts the EquipInventoryData structural invariants (`seg1 <= span`, plausible+aligned segment
     bases, last node readable at stride 0x18) before `strip_goods`/`goods_count` trust the chain. Latches
     the OK result (process-stable → ~free after first pass); a conclusive violation logs `[INVLAYOUT]
     … FAILED` once and DISABLES the strip. Live-verified: `canary OK (seg1=384 span=435)`, strip
     round-trips. This is the reference pattern for the remaining write-struct canaries.

## Tier A — as insidious as RVA, but the LARGEST surface (silent, undetected)

4. **Hardcoded struct field offsets** (`+0x98`, `+0x220`, …). ~**1069** literals across `src/`; densest:
   `worldmap_probe.cpp` (236), `icon_harvest.cpp` (191), `collected.cpp` (164), `msbe_parser.cpp` (119),
   `geom_move.cpp` (73). The RVA backlog EXCLUDES these as "stable" — true WITHIN one game build, but
   across an ER patch (or a struct-reshaping mod) an offset reads GARBAGE with **no health check and
   often no crash** — just a wrong marker / wrong item / wrong pointer. More insidious than an RVA
   (undetected), individually lower-severity (mostly reads). Unhardenable 1-by-1 at this count.
   **Mitigation:** not mass-annotation — add a handful of **layout canaries** on the hottest structs
   (GameDataMan→EquipInventoryData chain, WorldGeomIns transform, the marker/converter structs): assert
   one sentinel field (a known constant / self-pointer / vtable) at boot, so a shifted layout fails LOUD.

## Tier B — different axis: mod-agnostic correctness (prime-directive)

5. **ERR-specific hardcodes** (`MENU_MAP_ERR_*`, goods-id bands, fmg slots 10/419, `_ERR_` names) in
   `icon_harvest.cpp` (39), `overlay.cpp`/`map_renderer.cpp` (4 each), `custom_items.cpp`,
   `panel_dev_icons.cpp`, `grace_layer.cpp`. Not a crash risk — a SILENT WRONG RESULT on a non-ERR mod,
   which violates the prime directive (mod-agnostic first). Already the standing "runtime/disk over
   baked" workstream; tracked here only so it sits on the same danger map.

## Tier C — thread affinity (a hazard, not a "primitive")
Engine calls on the wrong thread deadlock/freeze (proven: `spawn_asset` off-present-thread deadlock on
the reqMgr RB-tree; warp mid-ImGui-draw freeze). Not patch-fragile, but a latent crash/hang class. The
mitigation is discipline (call on the game-update thread), tracked in the geom-spawn + heightfield items.

## QueryPerformanceCounter — verdict: leave it (not a hardening concern)
6 QPC calls: `sidecar.cpp:78` (GUID entropy = `QPC ^ tick ^ pid`) and `loot_open_probe.cpp` (5 — µs
file-open latency for the `[BOOTIO]`/`[MAPOPEN]` diagnostics). All are **stable Win32** — unlike an RVA
they do not drift on a game patch, do not write, and cannot silently corrupt. They ARE replaceable with
`std::chrono::steady_clock` (which on Windows is implemented over QPC anyway, same precision) purely for
portability/cleanliness, but there is **no correctness or hardening benefit** — this is busywork, not a
fragile primitive. Recommend: leave unless a broader cross-platform pass wants `<chrono>` everywhere.

## Suggested order (highest danger-per-effort first)
1. ~~The 2 grace-suppression **hooked RVAs** (tier S-2)~~ ✅ DONE 2026-07-04 (flag-gated dump, not trampoline).
2. The 2 engine **vtable slots** (tier S-1: geom setter 26, grace SetTo vt[1]) — resolve/assert from a ctor AOB. ← NEXT
3. A few **layout canaries** on the hottest write structs — ✅ inventory strip DONE (the reference
   pattern); remaining: WorldGeomIns transform (+0x220 move write) + the hottest read chains (tier A).
4. Continue the RVA backlog's PHYSWORLD slot + vtable/slot set.
Field-offset mass-annotation and QPC→chrono are explicitly NOT worth doing.
