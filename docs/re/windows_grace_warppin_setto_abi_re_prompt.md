# RE prompt — exact ABI for the SetTo "hide Icon_0" draw-only suppression (it crashed)

## Context

Tactic A from `windows_grace_warppin_teleport_re_findings.md` §4 — hook `vt[1] SetTo
FUN_14087ae20`, then for a discovered WarpPinData hide ONLY its `"Icon_0"` GFx child
to suppress the native grace icon while keeping the widget `_visible` (teleport).
Implemented (commit 7d62d8c) **and it CRASHED in-game.** Need the exact calling
convention to do it safely; the §4 pseudocode was a hint, not a verified ABI.

## The crash (the data to work from)

`exception_code = 0xC000001D` (**ILLEGAL_INSTRUCTION**), faulting at eldenring.exe
`+0x32EAF7C` (a high/non-.text address = jumped to garbage). Stack, top-down:

```
eldenring.exe +0xD7F88C   <- inside FUN_140d7f850 (release proxy) +0x3c   ← fault here
eldenring.exe +0x73334F   <- inside FUN_140733340 (set _visible) +0xf
MapForGoblins.dll +0x92553  <- our warp_setto_detour
MapForGoblins.dll +0x4D2B8E
eldenring.exe +0x74A2F0   <- FUN_14074a2f0 (get child) entry
... (engine SetTo / map refresh frames above)
```

So `get_child` returned, `set_visible` ran, then `release` (`FUN_140d7f850`) read a
function/vtable pointer out of our stack proxy buffer and **called a garbage
address**. ⇒ the stack proxy we pass is NOT a valid GFx object: wrong layout / wrong
init / wrong signatures / wrong string encoding.

## What we did (the guess that crashed)

```c
using getchild  = void*(__fastcall*)(void* widgetRoot, void* outChild, const char* name);
using setvis    = void*(__fastcall*)(void* child, int visible);
using release   = void*(__fastcall*)(void* child);
// in warp_setto_detour, after orig, for discovered grace pin:
alignas(16) uint8_t child[256] = {};            // over-allocated, zeroed
get_child(widgetRoot, child, "Icon_0");          // FUN_14074a2f0
set_visible(child, 0);                           // FUN_140733340
release(child);                                  // FUN_140d7f850  ← crashes
```

## What we need — the EXACT vanilla call, copied verbatim

Decompile **`FUN_14087ae20` (SetTo)`'s own call** to get-child / set-visible /
icon-image / release (the §2/§4 path with `local_78[40]` / `local_50[56]`) and report:

1. **`FUN_14074a2f0` (get child):** exact signature + calling convention. Args in
   order (is it `(widgetRoot, outProxy, name)` or different?). What is the **name
   param type** — UTF-8 `char*`, UTF-16 `wchar_t*`, a GFx/FName handle, or an
   interned id? Is `"Icon_0"` the right key (vs `"IconImage"`)? What EXACT bytes
   does it write into `outProxy`, and the proxy's **real size + alignment** (decode
   `local_78[40]`/`local_50[56]` — bytes or elements? one proxy or two?). Does the
   proxy need any pre-init before the call?

2. **`FUN_140733340` (set _visible):** signature — does it take the proxy BY POINTER
   (`&proxy`) or the inner widget pointer (`proxy.something`)? Is the 2nd arg an
   `int`, a `bool`, or a byte? (§1 said `FUN_140733340(widget, *(byte*)(pin+0xC))`.)

3. **`FUN_140d7f850` (release):** what does it read from the proxy (a refcounted
   GFx object at proxy+0x?) and how must the proxy be shaped so release is safe?
   This is where it crashed → the proxy's first qword is being used as a vtable/fn
   ptr. Give the field it derefs.

4. **The exact, paste-ready C** for "get the Icon_0 child of `widgetRoot`, set its
   `_visible=0`, release" — with the right proxy type/size, arg types, and string
   encoding — matching what SetTo itself does. Include the proxy struct definition.

5. If the get-child→set-visible→release dance is inherently fragile from a detour,
   give the **safer alternative**: hook the icon-image setter `FUN_14074bcc0`
   (`0x74bcc0`) and feed an INVALID descriptor (`d[0]=frame<1` or zero rect) so the
   engine hides the IconImage itself (§2) — but then how do we FILTER to grace pins
   only (the descriptor/iconChild don't carry the pin)? Is there a way to know,
   inside FUN_14074bcc0, that the caller is a discovered WorldMapWarpPinData (e.g. a
   thread-local set by our SetTo detour, or a field on iconChild)?

## Deliverable

`windows_grace_warppin_setto_abi_re_findings.md`: the verified signatures + proxy
struct (size/layout/init) + the paste-ready C for the Icon_0 hide, OR the
FUN_14074bcc0 invalid-descriptor route with a working grace-only filter. App
2.6.2.0 / ERR 2.2.9.6, imagebase 0x140000000; resolve by AOB (ASLR).

## Current safe state
The SetTo hook is DISABLED in code (`kSetToHookEnabled=false`, commit pending) →
no crash; `grace_suppress_native` is a no-op, hybrid draws+teleports discovered
graces. Re-enable once this ABI is verified.
