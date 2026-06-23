# RE findings — mid-session resolution change corrupts the world map (no-restart fix)

Answers `docs/re/windows_midsession_resolution_swapchain_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `find_screenmode.java` / `find_screenmode2..4.java` /
`find_resize.java` / `find_resize2.java` / `find_rendcfg.java` / `find_rendcfg2.java` /
`find_applyres.java`). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, MSVC `.text`.
Resolve by AOB; RVAs are reference for this build. Entry point = the user's clue, the
**"Screen Mode"** string that drives resolution from the in-game Graphics menu.

---

## 0. TL;DR  (★ updated after the user clue: the 3D world is ALSO zoomed)

> The corruption is **not** map-only — the **native 3D world stays zoomed** after a mid-session
> 1920×1080→1280×720 (both 16:9, so it's a pure **~1.5× scale error = oldW/newW**). That rules
> out the GFx-stage-only theory and points at the **shared ACTIVE render dimensions** that BOTH
> the 3D viewport AND the map zoom-fit read. **This is brief hypothesis 1, now located exactly.**

- **The stale value = the active render-target W/H** at
  **`[DAT_1447ef360 + 0x128 (output-list) + outIdx·0x170] + 0x118 (W float) / +0x11c (H float)`**.
  It is read by the map zoom-fit **`FUN_140d84990`** (`fVar1=*(entry+0x118); fVar3=*(entry+0x11c)`)
  and by the 3D viewport. Stale here ⇒ viewport sized 1920×1080 into a 1280×720 backbuffer ⇒
  **1.5× zoom in 3D and a wrong map fit, persistently.**
- These active dims are computed by **`FUN_1419ebb40(renderMgr, outIdx)`** (writes
  `entry+0x108/0x10c` int W/H, `+0x110/0x114` offsets, **`+0x118/+0x11c` float W/H**) from the
  per-output **source** dims `renderMgr + outIdx·0x30 + 0x3c8 (W) / +0x3cc (H)` and render-scale
  `+0x3ec`. (`outIdx = 0xffffffff` ⇒ recompute **all** outputs.)
- The resolution **apply is deferred**: per-frame driver `FUN_140e898d0` →
  **`FUN_1419eac90(DAT_1447ef360, 0, &resWH, 0)`** stages the new W/H to `entry+0x144` and sets
  two dirty flags (`entry+0x140`, `renderMgr+0xeb8`). The render-thread consumer
  **`FUN_1419ed870`** (`if(renderMgr+0xeb8) …`) only re-derives **entries whose `+0x140` dirty bit
  is set**; the full re-apply **`FUN_1419ed440(renderMgr, W, H)`** re-derives all outputs.
- **Root-cause hypothesis (matches the 3D+map symptom):** the mid-session resize marks/refreshes
  the **primary swapchain output** but leaves the **other render-output entries** (the 3D scene
  view / the UI-map view) with their `+0x140` dirty bit unset ⇒ `FUN_1419ed870` skips them ⇒ their
  active `+0x118/+0x11c` stay at the old resolution ⇒ zoom everywhere that reads them.
- **The map-projection fns themselves are clean:** `FUN_1409bdc50`/`FUN_1409ce190` read only
  CONSTANTS (`1920`=`DAT_14329e6f8`, `1080`=`DAT_14329e6f4`, `0.5`=`DAT_14329e660`); the only
  res-dependent path into them is the GFx fit `FUN_140d84990`, which reads the stale `entry+0x118`.
- **Fix (clean, in preference order):**
  1. **`FUN_1419ebb40(DAT_1447ef360, 0xffffffff)`** — recompute active dims for ALL outputs from
     source config. **No swapchain side-effects** (pure field recompute) → safest. Try first.
  2. **`FUN_1419ed440(DAT_1447ef360, newW, newH)`** — full re-apply to all outputs (updates source
     dims + recompute). More thorough; touches the swapchain mode-test → heavier.
  3. Raw-poke `entry+0x118/+0x11c` (+ int `+0x108/+0x10c`) — last resort, leaves derived fields.

---

## 1. Entry point — the "Screen Mode" string → `CSScreenModeCtrl`

`find_screenmode.java` scanned the image for the Graphics-menu strings. The live hits:
- **`CSScreenModeCtrl`** reflection/RTTI strings @ `0x142bfcd2a` ("CSScreenModeCtrl::THIS_CLASS"…)
  and `0x143d0b150` (mangled `?AV?$CSStepLocal@VCSScreenModeCtrl@CS@@@CS@@`). This is an **FD4Step**
  (`CSStepLocal<CSScreenModeCtrl>` over `FD4StepTemplateBase`) — the step that **applies a screen-
  mode/resolution change** over several frames.
- `CSGraphics` / `CSGraphicsStep` registration strings @ `0x142b64fc0…` (DLRuntimeClass registrars
  `FUN_1400a5680` etc.) — the graphics-config reflection class.
- `ScreenModeCtrl` / `ResolutionX` / `screenResolutionY` field names appear in reflection tables
  (RVA-referenced, not LEA), i.e. the serialized **`GraphicsConfig.xml`** fields.

`CSScreenModeCtrl` vtable (resolved via RTTI walk, `find_screenmode3.java`):
`vt[0]=FUN_140e8a700` (singleton get), `vt[2]=FUN_140e89140` (FD4 step-state driver),
`vt[3]=FUN_140e89770` (`isFinished`: `+0x40 == -1`), `vt[4]=FUN_140e8a0c0` (setState).
The **actual** per-state apply logic is dispatched from `FUN_140e89140` via the step-method table
at `this[1]` (`(**(this[1] + state*0x10))(this)`), not via the vtable. We did not need to unroll
that table — the apply funnels into the display subsystem below.

---

## 2. The map-projection math is resolution-CONSTANT except for the GFx stage rect

`find_screenmode2.java` decompiled the two fns the brief flagged:

### `FUN_1409bdc50` (pan)
Reads only **constants**: `DAT_142a950c0` (default pan vec2), `DAT_14329e6f8=1920.0`,
`DAT_14329e6f4=1080.0`, `DAT_14329e678=1.0`, `DAT_14329f470=0x80000000` (sign flip). The
resolution-dependent value is the **viewport rect read off the object returned by
`FUN_140d2e710`**:
```c
FUN_140d2e710(&obj);                      // obj = GFx stage/view object (via DAT_143d81ee8)
uVar8 = *(ushort*)(obj + 0xee);  uVar6 = *(ushort*)(obj + 0xea);   // h? / x?
uVar7 = *(ushort*)(obj + 0xec);  uVar9 = *(ushort*)(obj + 0xe8);   // w? / y?  (x/y/w/h rect)
```

### `FUN_1409ce190` (pan-bounds)
Reads `DAT_143d6cfc3` (surface/UG byte), `DAT_14329e660=0.5` (the marker→render SCALE),
`DAT_14329e6d8=100.0`, the snap/full rect at `param_1+0x340..0x370`, and the **scale divisor**
`FUN_1409e67e0(0)`. `FUN_1409e67e0` is a **static LUT**: `return *(float*)(&DAT_142b324b0 + i*4)`
with `[0]=0.198…, [1]=0.748…, [2]=2.25` — a fixed UI/aspect scale table, **not** backbuffer dims.

**Conclusion (refined in §4a after the 3D-zoom clue):** the engine's map math is virtual-canvas-
relative (1920×1080 const). It reaches the live resolution through the GFx fit
**`FUN_140d84990`**, which reads the **active render dims `[DAT_1447ef360+0x128]+0x118/+0x11c`** —
the SAME field the 3D viewport uses (so a stale value zooms both). The `object+0xe8` GFx stage rect
is a *secondary* consumer, not the root. Our overlay's `realW/1920` factor (`project_uv`) reads the
backbuffer live each frame, so the mod side is fine.

### `FUN_140d84990` — the GFx fit reads the active render dims
```c
lVar2 = *(longlong*)(DAT_1447ef360 + 0x128);   // first render-output entry
fVar1 = *(float*)(lVar2 + 0x118);              // active render W   ← stale after mid-session resize
fVar3 = *(float*)(lVar2 + 0x11c);              // active render H
// letterbox/pillarbox fit of the movie aspect vs fVar1/fVar3 → map render scale
```
This is the bridge proving map-fit and 3D share one stale field — see §0 / §4a / §5.

### What is the `+0xe8` object?
`FUN_140d2e710` → `FUN_140d4cc50(DAT_143d81ee8, 0x8f, 0, …)`, where `FUN_140d4cc50` is a bounds-
checked table getter `return *(qword*)(mgr + 0x88 + (id*9 + slot)*8)` (id `0x8f` < `0xC2`). So
`DAT_143d81ee8` is a **GFx/FD4 render-resource manager**, and the `+0xe8` viewport belongs to the
GFx **stage/movie view** for slot `0x8f`. This is the Scaleform stage size — refreshed (or not) by
whatever notifies GFx on a swapchain resize.

---

## 3. ER's display/resolution subsystem (`0x140e8xxxx`–`0x140e9xxxx`)

`find_resize.java`/`find_resize2.java` found the DXGI/Win32 resize APIs all called from this band.
Two singletons:
- **`DAT_1445894f8` = window manager** (read by every apply fn; FD4Singleton).
- **`DAT_1447ef360` = render/display-device manager** (= projection-doc `eldenring.exe+0x47ef360`;
  holds the render-output list). Heavily read, no static writers (constructed via computed ptr on
  the render-init/thread path).

### `FUN_140e8f910` — resolution source per screen-mode
```c
default {w=0x780(1920), h=0x438(1080)}                  // hard fallback = 1920×1080
mode 0 windowed   : FUN_140e73bf0(DAT_1445894f8, &wh, cfg+0x6c)   // windowed res index
mode 1 fullscreen : FUN_140e73a60(DAT_1445894f8, &wh, cfg+0x74)   // fullscreen res index
mode 2 borderless : FUN_140e978c0(&rect, cfg+0x66)               // monitor rect (GetSystemMetrics
                                                                  //  / display enum via DAT_1447ef360)
```

### `FUN_140e89a70` — window reposition on apply
Reads the **screen-mode config struct** `cfg`: **`+0xAC`=screenMode** (0/1/2), **`+0xC0`=width**,
**`+0xC4`=height**, `+0xB0/0xB4`=X/Y, `+0xD4`=display idx. Does `AdjustWindowRect` +
`SetWindowLongW` + `SetWindowPos`, and `FUN_140e73cd0(DAT_1445894f8, width)`.

### `FUN_140e898d0` — the per-frame apply driver (★ the resize funnel)
```c
if (cfg+0xb9 enabled && (cfg+0xbc debounce ticks down to 0) && DAT_1447ef360 != 0) {
    if (cfg+0xb8 dirty || modeChanged) {
        FUN_1419eac90(DAT_1447ef360, 0, cfg+0xc0, 0);   // ★ stage new res + set dirty flags
        SetActiveWindow; SetFocus; cfg+0xb8 = 0;
        if (ok) cfg+0xbc = 10;                          // 10-frame debounce
    }
    ... IsIconic/ShowWindow handling ...
    FUN_140e89fb0(cfg);                                 // → window reposition step
}
```

---

## 4. ★ Root mechanism — `FUN_1419eac90` is a DEFERRED-apply stager

```c
// FUN_1419eac90(renderMgr /*DAT_1447ef360*/, int outputIdx, undefined8* resWH /*cfg+0xc0*/, p4)
ulonglong FUN_1419eac90(longlong mgr, int outputIdx, undefined8* res, undefined8 p4) {
  e = *(ulonglong*)(mgr + 0x128);                       // render-output list BEGIN
  while (e != *(ulonglong*)(mgr + 0x130)) {             //          ...      END, stride 0x170
    if (*(int*)(e + 0x128) == outputIdx) {              // match this display/output
      *(qword*)(e + 0x144) = res[0];                    // stage: width|height (packed 2×int32)
      *(qword*)(e + 0x14c) = res[1];
      *(qword*)(e + 0x154) = res[2];
      *(qword*)(e + 0x15c) = res[3];
      *(int*)  (e + 0x164) = (int)res[4];
      *(byte*) (e + 0x140) = 1;                         // ★ per-output "pending resize" dirty flag
      *(qword*)(e + 0x168) = p4;
      *(byte*) (mgr + 0xeb8) = 1;                       // ★ manager "re-apply next render tick"
      return 1;
    }
    e += 0x170;
  }
  return 0;
}
```

So the apply does **not** synchronously resize anything — it stages the resolution descriptor
(~0x24 bytes at `cfg+0xc0`) into the matching render-output entry and raises two dirty flags. **The
render thread reads `mgr+0xeb8` next tick, walks the outputs, and on `entry+0x140` performs the
real swapchain resize + downstream re-derive** (this is the path that fires our DXGI
`ResizeBuffers` hook). The render thread side is VMProtect'd / dynamically dispatched → not
statically traceable here.

## 4a. ★ The consumer + the active-dims recompute (corrected mechanism)

The render thread reads `mgr+0xeb8` and runs **`FUN_1419ed870`** (AOB
`40 55 57 48 8D 6C 24 B1 …`): `if (mgr+0xeb8) { mgr+0xeb8=0; for each output entry, if (entry+0x140
dirty) promote pending entry+0x144 → source dims mgr+outIdx·0x30+0x3c8/0x3cc … }`. The active dims
are then (re)computed by **`FUN_1419ebb40(mgr, outIdx)`** (AOB `48 8B C4 55 41 54 41 55 41 56 41 57
48 8D A8 B8`):
```c
// per matching output entry:
W = fit(source mgr+0x3c8, aspect mgr+0x3e0/0x3e4) * renderScale(mgr+0x3ec);  // rounded to mult of 4
*(int*)  (entry+0x108) = W;   *(int*)  (entry+0x10c) = H;     // int W/H
*(float*)(entry+0x110) = offX; *(float*)(entry+0x114) = offY; // letterbox offsets
*(float*)(entry+0x118) = (float)W;  *(float*)(entry+0x11c) = (float)H;  // ★ active float W/H
```
`outIdx == 0xffffffff` ⇒ recompute **all** outputs (used by the full re-apply `FUN_1419ed440`).

**Why fresh-launch works but mid-session corrupts (matches the 3D+map symptom):** on first launch
every render-output entry is built/recomputed at the real res. On a mid-session resize the apply
(`FUN_1419eac90`) + consumer (`FUN_1419ed870`) only refresh the entries whose `+0x140` dirty bit is
set — evidently the **primary swapchain output only**. The other render-output entries (the 3D scene
view and/or the UI-map view) keep their active `+0x118/+0x11c` at the old resolution ⇒ their
viewports are sized 1920×1080 into a 1280×720 backbuffer ⇒ **persistent ~1.5× zoom in the 3D world
and a wrong map fit**, both from the one un-refreshed field. (The earlier "3D unaffected / GFx-stage-
only" hypothesis is REFUTED by the user's observation that the 3D world is also zoomed.)

---

## 5. Deliverable

### 5.1 Root cause
The stale state is the **active render-target dimensions** (shared by the 3D viewport AND the map
zoom-fit), at **`[DAT_1447ef360+0x128 + outIdx·0x170] + 0x118 (W float) / +0x11c (H float)`**. They
are produced by **`FUN_1419ebb40(renderMgr, outIdx)`** from the per-output source dims
`renderMgr+outIdx·0x30+0x3c8/0x3cc` + render-scale `+0x3ec`. The mid-session apply
(`FUN_1419eac90`) stages the new res to `entry+0x144` + sets dirty `entry+0x140` / `renderMgr+0xeb8`;
the render-thread consumer `FUN_1419ed870` only re-derives entries whose `+0x140` is set. **The 3D
world AND the map both staying zoomed ⇒ at least one render-output entry's active `+0x118/+0x11c`
is not refreshed on the mid-session path** (its dirty bit never gets set, or the recompute is
skipped for it). One stale field, both symptoms.

### 5.2 Fix functions (engine routines to call from our hook)
- **`FUN_1419ebb40(DAT_1447ef360, 0xffffffff)`** — recompute active dims for **all** outputs from
  the source config. **No swapchain side-effects** (writes only `entry+0x108/0x10c/0x110/0x114/
  0x118/0x11c`). Safest; the right first try. AOB `48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 B8`.
- **`FUN_1419ed440(DAT_1447ef360, newW, newH)`** — full re-apply: per output, update source dims
  `mgr+0x3c8/0x3cc` then `FUN_1419ebb40(mgr, -1)`. More thorough; calls the swapchain mode-test
  `thunk_FUN_141e9d290` → heavier/riskier from the hook. AOB
  `40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 5D`.
- Don't bother re-calling `FUN_1419eac90` (the stager): the in-game Apply already called it (that's
  why our `ResizeBuffers` fired), so re-staging + re-flagging reproduces the same gap.

### 5.3 C++ sketch wired into `hk_resize_buffers` (goblin_overlay.cpp:2091)
```cpp
// renderMgr singleton: DAT_1447ef360 is a .data global holding the ptr →
//   void* renderMgr = *(void**)(er_base + 0x47ef360);   // re-find slot by AOB per patch
// Recompute fn FUN_1419ebb40(renderMgr, outIdx): outIdx = -1 ⇒ all outputs.
using RecalcDimsFn = void (__fastcall*)(void* renderMgr, uint32_t outIdx);
using ReApplyFn    = void (__fastcall*)(void* renderMgr, uint32_t w, uint32_t h);
static RecalcDimsFn er_recalc_dims = /* AOB 48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 B8 */ nullptr;
static ReApplyFn    er_reapply_res = /* AOB 40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 5D */ nullptr;
static void**       er_render_mgr_slot = /* &DAT_1447ef360 */ nullptr;

HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {
    if (g_imgui_init) release_render_targets();
    HRESULT hr = o_resize_buffers(sc, count, w, h, fmt, flags);
    if (g_imgui_init && SUCCEEDED(hr)) { /* …existing RTV rebuild… */ }

    // EXPERIMENTAL no-restart re-derive of ER's cached render dims (config-gated; verify in §5.4):
    if (er_render_mgr_slot && *er_render_mgr_slot) {
        if (er_recalc_dims)
            er_recalc_dims(*er_render_mgr_slot, 0xffffffffu);          // (1) safest: recompute all
        // else if (er_reapply_res)
        //     er_reapply_res(*er_render_mgr_slot, w, h);              // (2) heavier full re-apply
    }
    return hr;
}
```
> Must run on the render thread (it’s the Present/ResizeBuffers thread — OK). `FUN_1419ebb40` and
> `FUN_1419ed440` both contain a `GetCurrentThreadId() == [mgr+0x6d0]+0x14` render-thread guard path
> in their callers; calling the recompute directly from the swapchain thread is the intended
> context. Keep it behind a config flag (`fix_midsession_resolution`, default OFF) until verified.

### 5.4 Decisive runtime test (quentin)
1. **Confirm the stale field.** Resolve `renderMgr = *(void**)(er_base+0x47ef360)`,
   `entry = *(void**)(renderMgr+0x128)` (first output), read `*(float*)(entry+0x118)` /
   `*(float*)(entry+0x11c)`. Before vs after a mid-session 1080→720: if they stay 1920/1080 while
   the swapchain `GetDesc` = 1280/720 ⇒ **confirmed**. Walk the whole list `[+0x128..+0x130]` stride
   `0x170` and log every entry’s `+0x118/+0x11c` + `+0x140` dirty bit — find which output stayed
   stale (that's the one the resize didn't mark). Add to `goblin_worldmap_probe.cpp`.
2. **Test fix (1):** call `FUN_1419ebb40(renderMgr, -1)` from the hook (config-gated). If the 3D
   zoom + map fit recover ⇒ ship it.
3. **If not:** test `FUN_1419ed440(renderMgr, w, h)` (full re-apply). If still not ⇒ the source dims
   `mgr+0x3c8/0x3cc` themselves are stale → update those first (from `w/h`) then recompute.

---

## 6. Version-stability (re-finding per patch)
Resolve by AOB (entry bytes, this build):
- `FUN_1419ebb40` (recompute active dims; arg2 outIdx, −1=all) — `48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 B8`.
- `FUN_1419ed440` (full re-apply; args W,H) — `40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 5D`.
- `FUN_1419ed870` (dirty consumer, reads `mgr+0xeb8`) — `40 55 57 48 8D 6C 24 B1 48 81 EC B8 00 00 00 48`.
- `FUN_1419eb800` (per-output apply) — `4C 8B DC 56 57 41 57 48 81 EC C0 00 00 00 48 8B`.
- `FUN_1419eac90` (stager) — `48 8B 81 28 01 00 00 4C 8B D9 4C 8B 91 30 01 00`.
- `FUN_140e898d0` (per-frame display driver) — `40 57 48 83 EC 20 80 B9 B9 00 00 00 00 48 8B F9`.
- `DAT_1447ef360` render-mgr slot: AOB the `48 8B 0D ?? ?? ?? ??` load near `FUN_1419e5980` /
  `FUN_140e898d0`, or compute `er_base + 0x47ef360` and deref.

Struct offsets (this build), render-output entry (stride `0x170`, list `mgr+0x128`begin/`+0x130`end,
key `entry+0x128 = outIdx`):
- **active dims** `entry+0x108`(int W)/`+0x10c`(int H), `+0x110/+0x114`(offset x/y),
  **`+0x118`(float W)/`+0x11c`(float H)** ← read by 3D viewport + map fit `FUN_140d84990`.
- **per-output dirty** `entry+0x140`; **staged/pending res** `entry+0x144..+0x164`.
- **source dims** `mgr + outIdx·0x30 + 0x3c8`(W)/`+0x3cc`(H); aspect `+0x3e0/0x3e4`; render-scale `+0x3ec`.
- **mgr dirty flag** `mgr+0xeb8`; render-thread id `[mgr+0x6d0]+0x14`.
- Screen-mode config (window mgr `DAT_1445894f8`): mode `cfg+0xAC`, w `cfg+0xC0`, h `cfg+0xC4`,
  descriptor base `cfg+0xC0` (~0x24 B), display idx `cfg+0xD4`.

Scripts (all in `D:\ghidra_scripts`): `find_screenmode.java`..`find_screenmode4.java`,
`find_resize.java`, `find_resize2.java`, `find_rendcfg.java`, `find_rendcfg2.java`,
`find_applyres.java`, `find_zoomfit.java`, `find_rendims.java`, `find_promote.java`,
`find_reapply.java`.
