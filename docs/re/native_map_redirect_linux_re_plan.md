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

## IMPLEMENTED 2026-07-06 (`c7ac663a`) — built green, NOT yet live-verified
Safe redirect coded: on the map-key edge with `vmap_on_map_key`, the vmap TOGGLES + force-closes the native
via `inject_native_map_close()` (SendInput **KEYEVENTF_SCANCODE** Escape — ER reads raw NOLEGACY, a wVk-only
send is dropped; this was the first live bug). Host flag `overlay_api::{set_,}vmap_redirect` keeps the vmap the
active fullscreen map after the native closes (`world_map_open()` false): `vmap_covers_map()` ORs it (input
stays locked+fed), `panel_virtual_map` Slice D gates the auto-close on `!s_redirected`. State machine:
map-key rising + redirect_mode → `!s_redirected` OPEN (open vmap, set redirect, inject close) / else CLOSE
(close vmap, clear redirect, inject close).

**★ LIVE-VERIFY PENDING — blocked by env, not code.** The verification pass was defeated by: input-injection
flakiness (`key m`/`key Escape` 'lost first send', so the map didn't reliably open/close), a messy multi-
instance game env (2+ eldenring.exe, stale RPC listener on 38700 held by wineserver, games not dying on
`pkill -9` — wine-resilient), and the 11h D-state husk. **Cleanest path: reboot the machine, single clean
boot (`hold_er.py`, profile `err_offline.me3`), then:** `set vmap_on_map_key 1` → `key m` (verify
`status map_open=0` = native force-closed) → screenshot (vmap fullscreen, native gone) → `key m` again (vmap
closes). If the native still shows, check the mod log for the inject + that Slice D's edge fired. Fallback if
the inject is still dropped: reuse the debug-RPC `key` verb's exact INPUT construction (it's proven to close
the map) instead of the hand-rolled SendInput.
- Env gotcha learned: `mfg.py rpc` one-shots need the holder to RELEASE its RPC socket (single-client); a
  stale listener makes a new boot connect to the dead socket (`RPC up ~0s` then ConnectionError). Kill ALL
  eldenring/me3/wineserver + confirm `ss -tlnp | grep 38700` is FREE before booting.

## SAFE METHOD REJECTED (user live-test 2026-07-06) → PIVOT to true NEVER-OPEN
User tested the safe redirect: **open → native map opens, then vmap covers it; close → vmap closes but the
native map stays OPEN.** The close-leaves-native-open is partly the scan-code inject fix not being in the
build under test (the running game predated `22f11d26`), BUT the user's verdict stands: **the open+close/flash
approach is not the right one** — they want the native to NEVER open. (Consistent with their earlier "pourquoi
ouvrir?".)

**⇒ Do the TRUE redirect: hook the OPENER + suppress the native open + toggle the vmap.**
- Hook target = the **opener function** (contains the fwa ret-addr `er+0x7efb89`; it sets up 4 args
  `mov r8,rbp; mov rdx,r12; mov rcx,rbx` then `call er+0x792550` which constructs the WorldMapDialog). Hooking
  the opener ENTRY and returning early when `vmap_on_map_key` = nothing is allocated/pushed → menu stack stays
  balanced (SAFE — unlike no-oping the ctor `er+0x9cf8f0`, which leaves the caller with an unconstructed
  object → crash). Both keybind systems (gamepad+kb) funnel through this opener → one hook covers both.
- In the hook: `vmap_on_map_key` on → TOGGLE the vmap (`virtual_map_open()`), set the redirect flag, and
  RETURN without calling the original (native never opens). Off → call original. The vmap is driven by the
  hook directly (not `world_map_open()`, which never fires when the native is suppressed).
- Reuse the already-built infra (`c7ac663a`): the host `overlay_api::vmap_redirect` flag + `vmap_covers_map()`
  OR + the input gating all still apply; only the trigger changes from "open+close on the map-open edge" to
  "the opener hook toggles + suppresses".

**★ BLOCKED on a clean env.** Pinning the opener ENTRY (scan back from `er+0x7efb89` for the prologue / author
an AOB) needs a LIVE game to read the module bytes (Linux `mem_dump`), and the hook needs crash-tolerant
live-testing. This session's env is unusable: no reachable game, multiple wedged eldenring/me3/wineserver that
survive `pkill -9`, a stale RPC port, and the 11h D-state husk. **NEXT SESSION: reboot the machine → clean boot
→ `er_base` + `mem_dump` around `er+0x7efb89` to pin the opener entry → author an AOB → MinHook it (suppress +
toggle) → live-test (freeze/fast-travel/page/fog).** The safe-redirect code stays committed as the reusable
flag/gate base; only the Slice-D trigger is replaced by the opener hook.

## OPENER PIN attempt (2026-07-06, Linux mem_dump) — byte-reading is UNRELIABLE, need the ctor's real caller
Read the module bytes around the fwa ret-addr `er+0x7efb89` (er_base 0x6ffff4c00000):
- The function containing `er+0x7efb89` = **`er+0x7efaf0`** (prologue `40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30`
  = push rbp/rsi/rdi/r12-r15; sub rsp,0x30). At `er+0x7efb84` it does `call er+0x792550`.
- BUT **`er+0x792550` is a GENERIC container op** (entry `49 3b d0 73 73 ... mov rbx,rdx; mov rdi,[rbx]; ...
  mov [rbx],rbp; add rbx,8` = a bounds-checked vector/list insert), **NOT the WorldMapDialog construction**.
  So the linear ret-addr scan does NOT give a clean caller→ctor chain — `er+0x9cfb6e` (ctor+0x27e) and
  `er+0x7efb89` are both on the stack but not directly caller/callee. **Byte-reading alone can't pin the
  world-map-specific "open" function.**

**⇒ Reliable next step = get the ctor's REAL immediate caller.** `mem_fwa` is DATA-only (can't exec-break the
ctor; the vtable global er+0x2b2d7d8 is read-only so fwa-write never fires, and fwa-read on it is drowned by
every method dispatch). So: **add a TEMPORARY diagnostic MinHook on the WorldMapDialog ctor `er+0x9cf8f0`
(AOB the verified prologue `48 89 4C 24 08 57 48 83 EC 30`) that logs its return address (the caller) once**,
rebuild + restart (boot_hold makes this cheap now), open the map, read the log → that caller IS the clean
"open world map" function. Confirm it's world-map-specific (only fires for the map, not other menus), then
MinHook it: `vmap_on_map_key` → open vmap + return early (suppress); else original. (Alternative: Windows
Ghidra call-graph.) Only after that is the true-redirect hook safe to wire — no-oping the wrong (generic)
function would break other menus.

**Infra ready this session:** `tools/boot_hold.py` + VSCode task "MFG: Boot ER + HOLD" (leave ER running,
socket released for `mfg.py rpc` one-shots). Killing the wedged env = `pkill -9 wineserver` (frees the stale
RPC port + all wine procs); the D-state husk is unkillable (needs a machine reboot) but is harmless.

## OPENER PINNED via Windows Ghidra call-graph (2026-07-06, build 2.6.2.0) — no ctor-hook needed
Took the "(Alternative: Windows Ghidra call-graph)" path instead of the diagnostic MinHook. Project
`D:\ghidra_proj2\ER` (imagebase `0x140000000`), tools `query.java` + bespoke `gap.java`/`refs.java` in
`D:\ghidra_scripts_mfg`. **Full static construction chain of `CS::WorldMapDialog`:**
```
menu-open (kb + gamepad both converge here)
  → dialog-factory dispatch table  er+0x2abb910  (WorldMapDialog slot)
    → FUN_1407fd4b0  (er+0x7fd4b0)   ★ THE REAL OPENER — world-map-only create-callback
       → FUN_1409cf940 (er+0x9cf940)  = factory: new(0x3ed0) then CALL the ctor
          → FUN_1409cef10 (er+0x9cef10) = THE REAL ctor (2519 bytes; calls setup FUN_1409be5e0)
```
- `FUN_1407fd4b0` (77 bytes): `x=FUN_140745280(arg…); x=FUN_1409cf940(x,8,*(arg+8)); FUN_140d7f850(cleanup);
  return x;`. **World-map-EXCLUSIVE** — its only real callee `FUN_1409cf940` allocates exactly `0x3ed0`
  (WorldMapDialog sizeof) and calls the WorldMapDialog ctor, so it can construct nothing else.
- **No static callers** (both `FUN_1407fd4b0` and the ctors are table-dispatched, not direct-called) —
  confirms the old "created via factory/vtable table" note. `FUN_1407fd4b0` is registered as the
  WorldMapDialog slot in the menu-dialog factory table **`er+0x2abb910`** (8-byte fn-ptr array; siblings
  `…7fb8d0, 7fe1b0, [7fd4b0], 7feee0, 7fc9d0…`). Both keybind systems funnel through this one table call →
  **ONE hook on `FUN_1407fd4b0` covers kb + gamepad.**

**⇒ HOOK TARGET = `FUN_1407fd4b0` (er+0x7fd4b0)** — the outermost world-map-only frame. MinHook its entry;
when `vmap_on_map_key`: toggle the vmap + set redirect + **return early without calling original** (nothing
allocated/pushed → menu stack stays balanced). Else call original. AOBs (resolve live, filter to the main
`.text`; both show 6 matches in the Ghidra image = the VMProtect duplicated-section artifact, unique in the
live process):
- `FUN_1407fd4b0`: `40 53 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 48 8B C2 48 8B D9`
- `FUN_1409cf940`: `40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC E0 00 00 00` (inner factory,
  equivalent deeper hook if the outer return-early crashes)

**Two corrections to earlier notes in this doc:**
- `er+0x9cf8f0` (prologue `48 89 4C 24 08 57 48 83 EC 30`), called "the VERIFIED ctor entry" above, is actually
  the WorldMapDialog **vector deleting destructor** (vtable slot 0: sets vftable, calls base, `if(flags&1)
  operator delete`) — NOT a constructor. Diagnostic-hooking IT for "the caller" would have watched teardown.
  The real ctor is `FUN_1409cef10`.
- The fwa byte-reading (`er+0x7efaf0`/`call er+0x792550`) fingered the WRONG function; the world-map open is
  table-dispatched through `0x7fd4b0`, not `0x7ef…`. And `er+0x9cfb6e` ("ctor+0x27e") is really inside
  `FUN_1409cfb60` = WorldMapDialog vtable slot [2] (per-frame step, calls `FUN_1409c32f0`), unrelated to
  construction — both were just co-resident stack frames during a map-open.

## ✅ DONE + LIVE-VERIFIED 2026-07-06 (`732fba1a`)
Implemented the TRUE never-open redirect via the Ghidra-pinned create-callback and verified it live on
Linux/Proton. `goblin_native_map_redirect.cpp` MinHooks `FUN_1407fd4b0` (er+0x7fd4b0, AOB `WORLDMAP_CREATE_CB`);
when `vmap_on_map_key`, the detour returns null without calling the original → the native WorldMapDialog is
never allocated/pushed, and it toggles `overlay_api::vmap_redirect`; `panel_virtual_map` Slice D mirrors
`s_open` to that flag. **Live test:** map key → `[VMAP-REDIRECT] suppressed`, `status map_open=0` (native never
opens), the vmap draws fullscreen (Limgrave/Liurnia/Caelid + markers + relief); map key again → vmap closes →
gameplay; `frame` keeps advancing = NO freeze. Both keybind systems covered (factory-table convergence).
- **AOB gotcha:** the bare prologue `40 53 48 81 EC 90 …` matched a sibling at er+0x779750 first; extended it
  with the `b2 08` + double-call body → unique to er+0x7fd4b0.
- **Follow-ups (not blocking):** verify vmap fast-travel/warp still works with the native suppressed (the map's
  #1 job; the vmap has its own grace-warp — confirm live), and the in-combat warp gate. The safe (open+close)
  method + its `inject_native_map_close` are removed.
