# RE findings — the SetTo "hide Icon_0" GFx proxy ABI (the crash was a one-line release-offset bug)

Answers `docs/re/windows_grace_warppin_setto_abi_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, script `re_v125`, decompile + raw disasm of `FUN_14087ae20`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.

---

## 0. TL;DR — the bug and the fix

Tactic A's get-child and set-visible calls were CORRECT. **The crash was the release
call: it must be passed `proxy + 0x28`, not the proxy base.** The GFx child proxy is a
two-part object — a `ComponentProxy` at `+0x00` and a **`CSScaleformValue` at `+0x28`** —
and the release fn `FUN_140d7f850` operates on the **`CSScaleformValue`**. Releasing the
base made `FUN_140d7f850` overwrite the `ComponentProxy` vtable with `CSScaleformValue`'s
and then call a method through a now-garbage pointer → `0xC000001D`.

```c
// BEFORE (crashed):  g_gfx_release_proxy(child);
// AFTER  (fixed):    g_gfx_release_proxy((uint8_t*)child + 0x28);
```

Verified against `FUN_14087ae20`'s own disasm: `get_child` writes the proxy at `RSP+0x40`;
the matching release is `LEA RCX,[RSP+0x68]; CALL FUN_140d7f850` → `0x68-0x40 = 0x28`.
Every proxy release in `SetTo` uses the same `+0x28` idiom (e.g. the final
`LEA RCX,[RSI+0x28]` releasing `widgetRoot+0x28`).

---

## 1. The proxy object (size / layout / init)

`get_child FUN_14074a2f0(widgetRoot, outProxy, name)` builds `outProxy` in place:
- `+0x00` `ComponentProxy::vftable` (ctor `FUN_1407330b0`; then overridden to
  `SceneObjProxy::vftable`); `+0x08/+0x10/+0x18` = self-referential list nodes
  (`p[1]=p[2]=p[3]=p`).
- `+0x20` (u32) flags = `*(u32*)(widgetRoot+0x20)`.
- **`+0x28` = a `CSScaleformValue`** (init by `FUN_14074a680(outProxy+5, …)`): its own
  `+0x00`=`CSScaleformValue::vftable`, `+0x18`(`[3]`)=held GFx ref (when flags bit6 set),
  `+0x20`(`[4]`)=flags, `+0x28`(`[5]`)=value. ~0x30 bytes → proxy spans ~`0x58–0x60`.

`SetTo`'s frame reserves `RSP+0x40 .. RSP+0xa0` (0x60) for it. **Over-allocating 256 B
zeroed is safe**; `get_child` fully initialises the parts release reads, in BOTH the
name-found and not-found branches (vanilla releases unconditionally after every
get_child, so an absent `"Icon_0"` is still release-safe).

## 2. The verified calling convention (copy of what SetTo itself does)

| fn | RVA | signature | notes |
|---|---|---|---|
| get child | `0x74a2f0` | `void* getChild(void* widgetRoot, void* outProxy, const char* name)` | name = **UTF-8 `char*`**; `"Icon_0"` is correct (string @ `.rdata`). Returns `outProxy`. |
| set visible | `0x733340` | `void setVisible(void* proxyBase, char visible)` | takes the **proxy base** (not +0x28). 2nd arg read as a **byte**. → `FUN_140d844d0(inner, v)`. |
| release | `0xd7f850` | `void release(CSScaleformValue* proxyPlus0x28)` | **call on `proxy + 0x28`**. Derefs `[+0x18]`/`[+0x20]bit6`/`[+0x28]`; sets `*p=CSScaleformValue::vftable`. |

`SetTo` disasm (the canonical sequence, `0x87ae5b..0x87ae8c`):
```
LEA  R8,[0x142a90040]        ; "Icon_0"
LEA  RDX,[RSP+0x40]          ; &proxy
CALL 0x14074a2f0             ; getChild(widgetRoot, &proxy, "Icon_0")
CALL [RDX+0x60]              ; vt[12](pin)  -> icon descriptor (vanilla then sets the image)
CALL 0x14074bcc0            ; setIconImage(proxy, desc)
LEA  RCX,[RSP+0x68]          ; &proxy + 0x28
CALL 0x140d7f850             ; release(&proxy+0x28)
```

## 3. Paste-ready C (the fixed Icon_0 hide)

```cpp
using gfx_get_child  = void *(__fastcall *)(void *widgetRoot, void *outProxy, const char *name);
using gfx_set_vis    = void *(__fastcall *)(void *proxyBase, int visible);
using gfx_release    = void *(__fastcall *)(void *cssvAtProxyPlus0x28);

alignas(16) uint8_t proxy[0x60] = {};            // zeroed; get_child fills it (0x60 ≥ real size)
g_gfx_get_child(widgetRoot, proxy, "Icon_0");     // build the child proxy
g_gfx_set_visible(proxy, 0);                       // hide the Icon_0 child (proxy BASE)
g_gfx_release_proxy(proxy + 0x28);                 // ← THE FIX: release the CSScaleformValue
```

All three are proven-safe calls (identical to vanilla `SetTo`). Hide via `set_visible` on
the child is sound — `FUN_140733340` does `inner = proxy->vt[1](); if((inner+0x20 & 0x8f)>1)
FUN_140d844d0(inner, v)` for any valid proxy. The outer pin widget keeps its own `_visible`
(set earlier in `SetTo` from `pin+0xC`), so the pin stays cursor-selectable → **teleport
works**. SetTo re-runs each refresh; our detour re-hides each time → persists.

## 4. The §5 alternative (not needed, but documented)

Hook `FUN_14074bcc0` (`0x74bcc0`) and feed an invalid descriptor (`d[0]=0`, zero rect,
`d[0xf]=-1`) so the engine hides the `IconImage` itself; filter to grace pins with a
**thread-local** set by the `SetTo` detour around its `orig()` call (SetTo→FUN_14074bcc0 is
synchronous on the engine thread, so the TLS scopes precisely to the grace's icon set). This
avoids constructing a proxy at all, but adds a second hot-path hook. **Prefer the fixed
tactic A in §3** — it's one line and uses only verified-safe calls.

## 5. Handles / RVAs (resolve by AOB; ASLR)
- SetTo (vt[1]) `FUN_14087ae20` `0x87ae20`; the get/release sequence at `0x87ae5b..0x87ae8c`.
- get child `FUN_14074a2f0` `0x74a2f0` (UTF-8 name, returns outProxy).
- set visible `FUN_140733340` `0x733340` → inner setter `FUN_140d844d0` `0xd844d0`.
- release `FUN_140d7f850` `0xd7f850` — **arg = proxy + 0x28** (a `CSScaleformValue`).
- proxy ctor `FUN_1407330b0` `0x7330b0`; `+0x28` CSScaleformValue init `FUN_14074a680` `0x74a680`.
- grace filter: `pin[0] == WorldMapWarpPinData::vftable` (`er+0x2ad8228`); discovered = `*(u32*)(pin+0x60) & 7`.
- §5 route: icon-image setter `FUN_14074bcc0` `0x74bcc0` (desc `int*`: `[0]`frame `[1..4]`rect `[0xf]`mode<0).
```
