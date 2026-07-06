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

## RECON DONE (2026-07-06, Linux mem_fwa) + refined hook point = the OPEN-menu CONVERGENCE
`mem_fwa` (write) on the map-open state byte **`CSMenuMan+0xCD`** (=7 when the world map is up; live addr this
boot `0x12a8474d`, `CSMenuMan=0x12a84680` via slot `0x6ffff896b7b0`; eldenring.exe base `0x6ffff4ba0000`).
Cycling `key m`/`key Escape` captured **7 distinct writers** of that byte (all in the menu system er+0x74xxxx:
er+0x7ad512, 0x744ed6, 0x745722, 0x746b77, 0x7457b4, 0x746812, 0x7adb87). +0xCD is a SHARED menu-screen-id
byte (every menu push/pop writes it) — NOT a per-dialog gate, so writing it directly would desync. The
**world-map-specific** open frames (hits #2/#4) share the caller chain **`er+0x9cfb6e → er+0x7efb89`**
(er+0x9cfb6e = WorldMapDialog ctor `er+0x9cf8f0` +0x27e; er+0x7efb89 = the menu code that opens it).

**User's key insight (2026-07-06):** ER has TWO keybind systems (gamepad + kb/mouse), BOTH mapped to open the
world map. So don't hook a raw key or either bind table — hook the **single call both converge to**: the
"open world map menu" function (**`er+0x7efb89`** region, the caller of the WorldMapDialog ctor). Both input
systems funnel through it, so ONE hook covers both automatically.

**⇒ REDIRECT hook (the real "never open"):** MinHook the opener function (entry of the `er+0x7efb89` /
ctor-caller). When `vmap_on_map_key` is on: **skip the native open** (return without constructing/pushing
WorldMapDialog) AND open the vmap (`virtual_map_open()=true`, mark `s_redirected`). When off: call original.
This ALSO fixes the trigger — the hook opens the vmap directly, not via `world_map_open()` detection (which
would never fire once the native is suppressed). Native never opens ⇒ never renders ⇒ no flash, no Scaleform
render, no A-style open/close.

**★ NEXT to implement:** pin the ENTRY of the opener function (the function containing/above `er+0x7efb89` that
calls the WorldMapDialog ctor `er+0x9cf8f0`) — read bytes around it (mem_dump) to find the prologue / author an
AOB, or hook the ctor `er+0x9cf8f0` and dump a deep stack to name the caller precisely. Then MinHook it,
gate on `vmap_on_map_key`, open the vmap + skip. **Live-test the freeze** (suppressing the open is the docs'
freeze caveat — but resident projection removed the VM coupling; if it desyncs, fall back to the SAFE
force-close: let it open then inject the game's own `Escape` close, which unwinds cleanly).

## PIN (2026-07-06, build 2.6.2.0) — WorldMapDialog ctor entry verified + the open call
- **`er_base` RPC gives the live base** (ASLR moves it per boot — DON'T derive base from a vtable read of a
  stale log; this boot `0x6ffff4c00000`). All `er+offset` from the fwa are base-relative and stable.
- **WorldMapDialog ctor = `er+0x9cf8f0`, VERIFIED entry** — prologue `48 89 4C 24 08 57 48 83 EC 30`
  (`mov [rsp+8],rcx ; push rdi ; sub rsp,0x30`), rcx = the dialog `this`. World-map-specific, known entry.
- The opener (function containing the fwa ret-addr `er+0x7efb89`) has, just before it, `call rel32` at
  `er+0x7efb84` → **`er+0x792550`** (the open-path helper that leads to the ctor), preceded by
  `mov r8,rbp ; mov rdx,r12 ; mov rcx,rbx` (a 4-arg call). So the opener is a menu-dispatch that sets up args
  then calls the world-map construction.

### Hook-behaviour decision (for the user — safe vs the "never open")
- **NEVER-OPEN (true redirect, what the user wants) = RISKY.** No-op'ing the ctor/opener leaves the menu
  system with no constructed dialog / an unbalanced push → crash/freeze unless we also intercept the menu
  push/pop + the caller's use of the returned object. Deep, needs crash-prone live iteration (4 crashes this
  session already; each = a flaky reboot).
- **SAFE render-cull (A, triggered precisely at the ctor) = PROVEN.** Hook `er+0x9cf8f0`; when
  `vmap_on_map_key`: let the ctor run (menu stays consistent), open the vmap, and force-close the native via
  the game's own close (proven: `key Escape` closes it, `map_open→0`, byte→0, clean unwind). Native opens for
  ~1 frame, INVISIBLE under the opaque vmap, then gone → no sustained Scaleform render, no freeze. Functionally
  the redirect, safely. Implementation = author an AOB for `er+0x9cf8f0` (base moves per boot), MinHook it,
  gate on `vmap_on_map_key`, decouple the vmap auto-close (`panel_virtual_map` Slice D `s_from_map` →
  `s_redirected`), + force-close.
- **RECOMMENDATION:** ship SAFE (A-at-ctor) first — it achieves "native never visibly renders" with zero freeze
  risk; then attempt true never-open as a gated experiment if the flash is ever perceptible (it isn't, under
  the opaque vmap).

## Follow-up — ER cursor render self-disables (needs a left-click to re-arm)
User-reported (2026-07-06): the ER mouse cursor's RENDER turns off after a while idle; a **left-click**
re-enables it. This matters for the vmap redirect (the vmap needs a live cursor). Same method as above —
another `mem_fwa` / find-what-reads target: arm on the cursor **visible/enable** byte (the game reads it each
frame to decide whether to draw the cursor) and watch what CLEARS it on idle + what SETS it on left-click. Then
the mod can keep it armed while the vmap is up (write the enable byte each present, like `movieclip_maintain`).
Candidate anchors: the cursor is `CS::WorldMapCursorControl`-adjacent (`CURSOR_VTABLE_RVA`, published as
`g_active_cursor`), but the OS/UI cursor render flag is likely a CSMenuMan / input-manager field — find it live.
Not blocking the redirect; do it alongside (both are `mem_fwa` recon on the running game).

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
