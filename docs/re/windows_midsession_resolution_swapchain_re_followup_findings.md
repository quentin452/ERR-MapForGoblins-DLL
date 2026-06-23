# RE findings — clean the swapchain on a mid-session resolution/mode change

Answers `docs/re/windows_midsession_resolution_swapchain_re_followup_prompt.md`. Static Ghidra
(`D:\ghidra_proj2\ER`, scripts `find_swapclean.java` / `find_swapclean2.java`, headless `-process`
reuse). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, MSVC `.text`. Resolve by AOB.

---

## 0. TL;DR — the raw-poke is the wrong layer; call `FUN_1419ed440(mgr, W, H)`

The mid-session resolution change does not just need new *numbers* — it needs **GPU resources
recreated**: the swapchain back-buffers (DXGI `ResizeBuffers`) **and** ER's per-output intermediate
render-target array. The raw-poke (fix-3) writes only scalar dims, so:
- **borderless same-aspect** looked fixed only because the in-game apply happened to leave usable
  targets; the dims poke made the fit read right.
- **windowed** still zooms because ER's per-output 3D/UI render targets are **recreated at the OLD
  size** (the same-mode apply path never refreshed the source dims they're built from — see §3).
- **fullscreen** doubles because the **swapchain back-buffers + the buffered render-target array
  `entry+0x90[]/+0xa8[]/+0xc0[]` are never released/recreated** on a same-mode change (see §4).

**`FUN_1419ed440(renderMgr, newW, newH)` is the complete re-apply** and is the recommended fix. It
unconditionally: bumps the generation counter, broadcasts a release to swapchain consumers, releases
**all** per-output GPU targets (`FUN_1419ec400(mgr,-1)`), calls `ResizeBuffers` on every output,
re-reads the desc into the source dims `mgr+0x3c8/0x3cc`, and recomputes+recreates every output's
targets (`FUN_1419ebb40(mgr,-1)`). One call fixes windowed, fullscreen and borderless.

> Call it **edge-triggered from the Present hook** (the render thread), NOT from `hk_resize_buffers`
> (it calls `ResizeBuffers` internally → re-entrant). Retire `fix_render_dims` (the poke).

---

## 1. Confirmed DXGI mapping (the wrappers the engine uses)

D3D12 swapchain wrapper; `*entry` (= `entry+0x0`) is the `IDXGISwapChain3*` (its vtable at `*entry`):

| engine fn | does | vtable call |
|---|---|---|
| `thunk_FUN_141e9d290` → `FUN_141e9d290(scWrap, count, W, H, fmt)` | **ResizeBuffers** + release/recreate per-buffer RTVs | `vtbl[0x68]` (idx 13 = `ResizeBuffers`); `vtbl[0x120]` (idx 36 = `GetCurrentBackBufferIndex`, IDXGISwapChain3) |
| `FUN_141e97920(sc, &modeDesc)` → `FUN_141e9d460` | **ResizeTarget** (output/window only) | resizes the *target*, not the buffers |
| `thunk_FUN_141e9d550(sc, BOOL fs, output)` | **SetFullscreenState** | `vtbl[0x50]` (idx 10) |
| `thunk_FUN_141e9d100(sc, &desc)` | **GetDesc** (fills 0x58-byte desc) | — |
| `FUN_141e92050(mgr+0x148, idx)` | bounds-checked **IDXGIOutput[idx]** getter | `return idx<[+0x10] ? [+8]+idx*8 : 0` |

This makes the consumer/re-apply flow legible: ResizeTarget ≠ ResizeBuffers, and only the latter
resizes the back-buffers.

---

## 2. Q1 — replicating the stager / dirty-flag path does NOT clean the swapchain. **Rejected.**

`FUN_1419ed870` (the `mgr+0xeb8` consumer) decompiled in full. The staged descriptor it reads at
`entry+0x144..+0x164` is:

| off | field | consumer use |
|---|---|---|
| `+0x144` | int **width** | `local_88`, `local_78` |
| `+0x148` | int **height** | `iStack_84`, `uStack_74` |
| `+0x14c` | int **refreshNum** | falls back to `0x3c`=60 when `mgr+0x1d1==0` or `+0x150==0` |
| `+0x150` | int **refreshDen** | fallback `1` |
| `+0x154` | int **mode** (`iVar1`; 0 = windowed/borderless, ≠0 = exclusive FS) | drives `bVar10` |
| `+0x158` | int **displayIndex** | `FUN_141e92050(mgr+0x148, idx)` → IDXGIOutput |
| `+0x15c` | int → promoted to source **`+0x3e0`** (aspect num) | |
| `+0x160` | int → promoted to source **`+0x3e4`** (aspect den) | |
| `+0x164` | float → promoted to source **`+0x3ec`** (render scale) | |

**The fatal gate:** inside the consumer's apply block,
```c
if (((local_94 == seq) && local_97) || ((iVar3 == seq) && bVar4)) {
    FUN_1419ea1e0(mgr);
    FUN_1419ec400(mgr, outIdx);          // release this output's targets
    if (bVar4)                           // ← ResizeBuffers ONLY here
        thunk_FUN_141e9d290(*entry, entry+0x34, W, H);
    FUN_1419ebb40(mgr, outIdx);          // recompute + recreate targets
    ...
}
```
`bVar4` is set **true only on a fullscreen-state transition** (the `*(char*)(entry+0x12c) != mode`
branch, or windowed-with-display-change). For a **same-mode resolution change** `bVar4` stays false
⇒ **`ResizeBuffers` is skipped**. Worse: because `ResizeBuffers`→`GetDesc` is the *only* writer of
the real source dims `mgr+0x3c8/0x3cc`, those are **never refreshed** on the same-mode path — the
consumer only promotes the *aspect* (`+0x3e0/0x3e4`) and *scale* (`+0x3ec`). So `FUN_1419ebb40` then
recreates the per-output targets from **stale** `+0x3c8/0x3cc`.

⇒ Re-staging + re-flagging reproduces exactly the bug we already see. **Q1 cannot work for
same-mode changes.** (It would only ever fix a true mode toggle, which the engine already handles.)

---

## 3. Q3 — why WINDOWED still zooms (the precise field)

`FUN_1419ebb40(mgr, outIdx)` (the active-dims recompute, sz=993) reads, per output `outIdx`:
- source **`mgr+outIdx*0x30+0x3c8`(W) / `+0x3cc`(H)** ← the real backbuffer dims,
- aspect **`+0x3e0`(num) / `+0x3e4`(den)**, render-scale **`+0x3ec`**,

derives active dims (rounded to mult-of-4, ×scale), writes `entry+0x108/0x10c`(int W/H),
`+0x110/0x114`(offsets), `+0x118/0x11c`(float W/H), `+0x124`=1.0 — **and then recreates the
per-output render-target views** via `thunk_FUN_141e9d220` (enumerate buffers) →
`FUN_141e862f0`/`FUN_141e86e60` (create targets at the new active size) and `GetDesc`
(`thunk_FUN_141e9d100`).

So windowed zoom is **not a scalar field the poke missed** — it is the **per-output 3D/UI render
target recreated at the OLD resolution**, because on the same-mode path the source `+0x3c8/0x3cc`
that `FUN_1419ebb40` builds from is never refreshed (ResizeBuffers skipped, §2). The dims-poke
writes `+0x3c8/0x3cc` *correctly* but never calls `FUN_1419ebb40`, so the GPU targets stay old.
The map fit `FUN_140d84990` reads `entry+0x118/0x11c/0x120` (we confirmed: `lVar2 = *(mgr+0x128);
fVar1=*(lVar2+0x118); fVar3=*(lVar2+0x11c)`) — those *are* poked, which is why the map sometimes
looked closer to right than the 3D view did.

**Resolution source per mode** (`FUN_140e8f910`, for reference): default `0x780×0x438`; mode 0
windowed `FUN_140e73bf0(winMgr,&wh,cfg+0x6c)`; mode 1 fullscreen `FUN_140e73a60(winMgr,&wh,cfg+0x74)`;
mode 2 borderless `FUN_140e978c0(&rect,cfg+0x66)` (monitor rect). Windowed render res = window client
= swapchain backbuffer, so `GetDesc` W/H is the right value to feed the fix.

---

## 4. Q4 — the doubled "two games" (the stale GPU resources)

`FUN_1419ec400(mgr, outIdx)` is the per-output **release** routine. For each entry it Unref/frees:
- `entry+0x100`, `entry+0xf8` (via allocator `DAT_1447ef690` vtbl+0x68), `entry+0xf0`, `entry+0xe8`,
- and the **buffered arrays** `entry+0xc0[i]`, `entry+0xa8[i]`, `entry+0x90[i]` for
  `i < entry+0xd8` — i.e. the **per-frame render targets / RTV-heap entries** (D3D12 frame buffering).

These are GPU resources sized to the resolution. On a same-mode change the consumer releases them
**only** when its gated block runs *and* recreates them from **stale** source dims (§2/§3); the
swapchain back-buffers themselves are not `ResizeBuffers`'d at all. The result is old-sized targets +
old-sized buffers presented under the new frame ⇒ the tiled/doubled image — even though every scalar
dim reads the new value (which is exactly what `[RENDIMS]` reported).

⇒ The "field behind the doubling" is not a scalar: it is the **per-output buffered render-target
array `entry+0x90[]/+0xa8[]/+0xc0[]` (count `entry+0xd8`) plus the DXGI back-buffers**, none of which
a memory poke can rebuild.

---

## 5. Q2 — `FUN_1419ed440(mgr, W, H)` is the complete re-apply. **Recommended.** ✅

Full decompile (sz=550):
```c
void FUN_1419ed440(mgr, W, H) {
  *(int*)(mgr+0xebc) += 1;                       // bump generation/seq
  // broadcast to swapchain-consumer listeners (mgr+0xe78 list): vtbl[+8]() then vtbl[+0x18]()
  //   → pre-resize RELEASE of external swapchain-dependent resources
  // thread guard: if GetCurrentThreadId()!=[mgr+0x6d0]+0x14 → FUN_141af34f0 + lock(mgr+0xfe0)
  FUN_1419ec400(mgr, 0xffffffff);                // release ALL per-output GPU targets (§4)
  for (entry e in [mgr+0x128 .. mgr+0x130) step 0x170) {
      if (thunk_FUN_141e9d290(*e, e+0x34, W, H) == 0) {   // ResizeBuffers — UNCONDITIONAL
          thunk_FUN_141e9d100(*e, &desc);                 // GetDesc
          // copy desc → entry+0x10..0x60 mirror
          *(int*)(mgr+0x3c8 + outIdx*0x30) = desc.Width;  // refresh SOURCE dims
          *(int*)(mgr+0x3cc + outIdx*0x30) = desc.Height;
          FUN_141e97950(*e, &st, 0); e[0x21]=0; *(bool*)(e+0x12c)= st==0;
      }
  }
  FUN_1419ebb40(mgr, 0xffffffff);                // recompute + recreate ALL outputs' targets (§3)
}
```
It does **not** read `cfg`, does **not** call `SetFullscreenState` — it resizes the *existing*
swapchain's buffers to `W×H` and re-derives everything. That is exactly right for a resolution change
within a mode (windowed and fullscreen-same-mode); it is *not* a mode toggle (the engine's own apply
handles fullscreen↔windowed). Passing the live backbuffer `GetDesc` W/H is correct for all modes, and
it self-corrects the source dims from `GetDesc` after the resize regardless of the W/H we pass.

### Safety / threading
- **Call from `hk_present`, never from `hk_resize_buffers`** — it calls `ResizeBuffers` internally,
  which re-enters our `hk_resize_buffers` (fine, our RTV rebuild runs) but would be re-entrant if
  fired from inside a resize.
- **Render-thread context is intended.** The guard `GetCurrentThreadId()==[mgr+0x6d0]+0x14` is true
  on the Present thread, so the lock branch is skipped. Gate the call on this equality to be safe.
- **No `SetFullscreenState` path** ⇒ no DXGI mode re-entry; the only re-entry is the nested
  `ResizeBuffers`. Add a static re-entrancy flag so the per-frame edge-trigger can't re-fire mid-call.
- It bumps `mgr+0xebc` and broadcasts release/recreate — must run with no command list recording;
  top of the Present hook (before `o_present`) is the right spot.

---

## 6. Deliverable C++ — Present-hook enforcer (edge-triggered)

```cpp
// goblin_worldmap_probe.cpp — replaces fix_render_dims (the poke).
using ReApplyFn = void (__fastcall*)(void* renderMgr, uint32_t w, uint32_t h);
static ReApplyFn  er_reapply_res    = nullptr;  // AOB 40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05
static void**     er_render_mgr_slot = nullptr; // &DAT_1447ef360 = er_base + 0x47ef360

// Call once per detected change from hk_present (NOT hk_resize_buffers).
bool reapply_render_res(IDXGISwapChain3* sc) {
    static bool s_in = false;
    if (s_in || !er_reapply_res || !er_render_mgr_slot) return false;
    void* mgr = *er_render_mgr_slot;
    if (!mgr) return false;

    // live backbuffer dims
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.BufferDesc.Width || !d.BufferDesc.Height) return false;
    const uint32_t w = d.BufferDesc.Width, h = d.BufferDesc.Height;

    // cheap edge-trigger: first output's active float dims vs live backbuffer
    uint64_t begin = 0; float aw = 0, ah = 0;
    if (!seh_read8((void*)((uintptr_t)mgr + 0x128), &begin) || begin < 0x10000) return false;
    seh_read4((void*)(begin + 0x118), &aw);
    seh_read4((void*)(begin + 0x11c), &ah);
    if ((uint32_t)aw == w && (uint32_t)ah == h) return false;  // already consistent

    // render-thread guard: [mgr+0x6d0]+0x14 == GetCurrentThreadId()
    uint64_t rt = 0; uint32_t rtid = 0;
    seh_read8((void*)((uintptr_t)mgr + 0x6d0), &rt);
    if (rt > 0x10000) { seh_read_i32((void*)(rt + 0x14), (int*)&rtid);
                        if (rtid && rtid != GetCurrentThreadId()) return false; }

    s_in = true;
    __try { er_reapply_res(mgr, w, h); }            // the complete re-apply
    __except (EXCEPTION_EXECUTE_HANDLER) { /* log */ }
    s_in = false;
    spdlog::info("[RENDIMS] reapply_render_res({}x{}) via FUN_1419ed440", w, h);
    return true;
}
```
Wire `reapply_render_res(swapchain)` near the top of `hk_present`, behind the existing
`fix_midsession_resolution` config flag. The nested `ResizeBuffers` it triggers will fire
`hk_resize_buffers` → our ImGui RTV rebuild, as today.

---

## 7. AOBs (this build) — version-stability

| fn | role | AOB (entry) |
|---|---|---|
| `FUN_1419ed440` | **full re-apply (W,H)** — the fix | `40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 5D D9 26 02 48` |
| `FUN_1419ed870` | dirty consumer (reads `mgr+0xeb8`) | `40 55 57 48 8D 6C 24 B1 48 81 EC B8 00 00 00 48 8B 05 2A D5` |
| `FUN_1419eb800` | per-output apply (uncond. ResizeBuffers) | `4C 8B DC 56 57 41 57 48 81 EC C0 00 00 00 48 8B 05 9B F5 26` |
| `FUN_1419ebb40` | recompute + recreate per-output targets | `48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 B8 FC FF FF 48` |
| `FUN_1419ec400` | release per-output GPU targets | `40 53 41 55 41 57 48 83 EC 30 48 8B 99 28 01 00 00 44 8B FA` |
| `FUN_141e9d290` | ResizeBuffers wrapper | `48 8B C4 44 89 40 18 53 41 54 41 55 41 56 48 81 EC 98 00 00` |
| `FUN_141e9d550` | SetFullscreenState wrapper | `48 89 74 24 10 57 48 83 EC 20 48 83 39 00 0F B6 FA 48 8B F1` |

- renderMgr slot `DAT_1447ef360` = `er_base + 0x47ef360` (deref for the mgr ptr).
- entry stride `0x170`, list `mgr+0x128`(begin)/`+0x130`(end), key `entry+0x128 = outIdx`.
- source dims `mgr+outIdx*0x30+0x3c8/0x3cc`; aspect `+0x3e0/0x3e4`; render-scale `+0x3ec`;
  refresh `+0x3f0/0x3f4`. active dims `entry+0x108/0x10c`(int) `+0x110/0x114`(off) `+0x118/0x11c`(float).
- per-output GPU targets: `entry+0xe8/0xf0/0xf8/0x100`, buffered arrays `entry+0x90[]/0xa8[]/0xc0[]`
  count `entry+0xd8`. staged res `entry+0x144..0x164`; per-output dirty `entry+0x140`; applied flag
  `entry+0x12c`. mgr dirty `mgr+0xeb8`, generation `mgr+0xebc`, render-thread id `[mgr+0x6d0]+0x14`,
  listener list `mgr+0xe78`, sync obj `mgr+0xfe0`.

Scripts: `D:\ghidra_scripts\find_swapclean.java`, `find_swapclean2.java`
(outputs `out_swapclean2.txt` / `out_swapclean3.txt`).
