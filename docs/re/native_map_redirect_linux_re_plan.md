# Native-map REDIRECT (not render-cull) — Linux runtime RE plan

**Strategy pivot (user, 2026-07-06):** stop trying to CULL the native map's Scaleform render (both cheap levers
dead — §4c/§4d of `windows_native_map_render_toggle_re_findings.md`; and the Route-1 A/B found no safe
render-gate field on the 0xe8 player — `windows_native_map_drawvfunc_re_findings.md` §7). Instead **REDIRECT**:
since the vmap projection is now **resident/off-VM** (the native map need NEVER be open — proven this session,
off-VM affine for all placed areas), we can let the game map key open the vmap and **force-close the native
`WorldMapDialog`** so it stops rendering entirely. Native closed ⇒ no Scaleform draw ⇒ the render-cull is
achieved with zero Scaleform RE.

**This is fully Linux-doable — NO Windows Ghidra prompt needed.** Everything required is already here:
- Map-open state anchor: **`world_map_open()` = `CSMenuMan[0xCD] == 7`** (goblin_tutorial_popup.cpp; `CSMENUMAN_SLOT`
  AOB resolves the singleton live). The screen-state byte flips 7 on open, other on close.
- **`mem_fwa`** RPC = hardware-breakpoint find-what-accesses (DR7, `goblin_field_probe.cpp`) — captures the exact
  instructions that WRITE a target address, live, on Linux/Proton.
- WorldMapDialog vtable **er+0x2b2d7d8** (13 slots, enumerated) + ctor `FUN_1409cf8f0`, CSMenuMan managers
  (`CSMENUMAN_SLOT`/`CSFEMAN_SLOT` pinned), the menu UPDATE tick `FUN_140766980` — all from the static RE.
- Live RPM + MinHook (the mod already detours many funcs on Linux).

## Plan
1. **Recon (mem_fwa).** Resolve `CSMenuMan` live, compute `&CSMenuMan[0xCD]`, arm `mem_fwa` (write). Open the
   map (`key m`) → capture the instruction that writes **7** (the OPEN transition) and, on `key Escape`, the
   instruction that clears it (the CLOSE transition). Those two call sites are the menu open/close of the
   WorldMapDialog screen. (Also worth: fwa on the CSMenuMan menu-stack pointer to catch the push/pop of the
   dialog object, which is the REAL open/close vs the descriptive 0xCD byte.)
2. **Find the CLOSE call.** From the close-transition site, walk up to the reusable "pop/close the world-map
   menu" call (the CSMenuMan pop of the `WorldMapDialog` — identify the dialog by vtable er+0x2b2d7d8). Prefer
   the highest-level close that tears down the dialog cleanly (same as the game's own Q/Escape close), so
   fast-travel/page/fog state unwinds normally (no freeze).
3. **Hook / invoke.** On the map-open edge with `vmap_on_map_key` (host already detects `world_map_open()`),
   **call that close** once — the native map opens then immediately closes (a 1-2 frame flash), the vmap stays.
   Alternative if a clean close call isn't isolable: hook the OPEN/push and no-op it (suppress the native open
   outright) — riskier (the findings' "suppress menu open → freeze" caveat), so try force-CLOSE first.
4. **Decouple the vmap auto-close.** Today `panel_virtual_map.cpp` Slice D closes the vmap when
   `world_map_open()` goes false (`s_from_map`). For the redirect the vmap must STAY open after we force-close
   the native → gate that auto-close off when we did the forced close (a new `s_redirected` flag), and route
   the vmap's OWN close (map key / Esc) to close the vmap instead.
5. **Live-test the freeze.** The design docs flag a "vmap map-context freeze" for suppressing map open/close —
   but that was tied to the projection needing the VM (map open once). Projection is resident now, so retest:
   after the redirect, confirm fast-travel (grace warp), page/dimension follow, fog/fragments, and repeated
   open/close all work with no freeze. If force-close desyncs, fall back to injecting the game's own close
   input (the cheap approach A) which unwinds exactly like a player Escape.

## Why not the render-cull anymore
`windows_native_map_drawvfunc_re_findings.md` §7: the movie is submitted by a central CSScaleform render pass
regardless of the player's fields; no safe pokeable gate exists (the two fields that change rendering crash the
present). Redirect sidesteps that entire subsystem — the native map simply isn't open, so there's nothing to
cull.

## Anchors
```
map-open state   CSMenuMan[0xCD]==7        (world_map_open(); CSMENUMAN_SLOT AOB)
fwa tool         mem_fwa RPC              (goblin_field_probe.cpp, DR7 hw-breakpoint, write-watch)
dialog           CS::WorldMapDialog vtable er+0x2b2d7d8 ; ctor FUN_1409cf8f0
menu managers    CSMenuManImp / CSFeManImp (CSMENUMAN_SLOT / CSFEMAN_SLOT) ; UPDATE tick FUN_140766980
vmap auto-close  panel_virtual_map.cpp Slice D (s_from_map) — must decouple for the redirect
```
Cross-ref: `windows_native_map_render_toggle_re_findings.md`, `windows_native_map_drawvfunc_re_findings.md` (§7
the A/B negative), `windows_worldmap_affine_resident_source_re_findings.md` (resident projection = why redirect
is now safe).
