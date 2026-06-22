# RE findings — recover world-map ICON textures at runtime (kill the FFDEC atlas)

Answers `docs/re/windows_runtime_icon_textures_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v66..v70`, output `out_v66.txt`..`out_v70.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only (game not running during this
RE → the resource-pointer reads and the iconId↔image binding are the **runtime** step, §7).

---

## 0. TL;DR

The icon sprite-sheet IS a live `ID3D12Resource`, and — crucially — **the per-sprite UV rect is
stored in a plain C++ field** (`Scaleform::Render::TextureImage`/`CS::CSTextureImage`), NOT only
inside the Flash timeline. So both brief unknowns are C++-readable once you hold the icon's
Scaleform image. The bridge:

```
GFx worldmap movie image  (built by CS::CSScaleformImageCreator from the imported TPF)
   └─ CS::CSTextureImage : Scaleform::Render::TextureImage      ← carries the SUB-RECT (#2)
        +0x38  ptr → CS::CSGxTexture (GXBS::IGXTexture)          ← the backing sheet
        +0x2c/+0x34 i32  full-sheet W/H
        +0x74/+0x78/+0x7c/+0x80 i32  rect x0/y0/x1/y1  (px in the sheet)
        // CORRECTED 2026-06-22 by the live [ICONTEX] probe — was +0x3c/+0x40 for y (wrong).
        // See windows_runtime_icon_textures_followup_re_findings.md §1.
   └─ CS::CSGxTexture  →  GXBS::GXTexture2D                      ← the concrete GPU texture (#1)
        +0x40  ptr → GXBS GPU-resource object → ID3D12Resource (desc at [+0x40]+0x20)
        +0x48  SRV / descriptor handle ;  +0x58 i32 format/flag
        vt6..vt9/vt15 = GetWidth/Height/Format/Depth (read [+0x40]+0x20) ; vt16 = IsValid (+0x40!=0)
```

- **Unknown #1 (the resource + resolve chain)** → `GXBS::GXTexture2D` (vtable RVA `0x2f05928`),
  `ID3D12Resource` reached via `GXTexture2D+0x40` → GPU-resource → desc at `+0x20`. Reached from a
  worldmap icon image by `CSTextureImage+0x38` → `CSGxTexture` → its `GXTexture2D`.
- **Unknown #2 (iconId→rect)** → the rect is on the `CSTextureImage` (`+0x74/+0x78/+0x7c/+0x80` —
  CORRECTED, live; full dims `+0x84/+0x88` & `+0x2c/+0x30`). It is **not** a flat `iconId→rect`
  table; it's per-Scaleform-image. You
  bind it to an iconId at the image, via `CS::WorldMapPointPinData::GetIconId` + the item control
  (Scaleform side, VMP) — so the iconId↔image map is captured at runtime, §7.
- **Readback** → `CopyTextureRegion` the sub-rect from the engine's `ID3D12Resource` into our own
  texture (GPU→GPU, keep it resident as an SRV — no CPU de-pitch/de-BCn needed for ImGui). §6.
- **Path A vs B verdict** → **Path A** (copy the D3D resource sub-rect). Path B (drive Scaleform to
  render a sprite to our RT) is natively supported (`CS::CSMovieGxTexture`, §5) but means driving GFx
  through the VMP wall; Path A only needs the resource + rect we already located. §5.

---

## 1. The Scaleform→D3D class bridge (all RTTI-named, found via symbol hunt)

| class | vtable RVA | role |
|---|---|---|
| `CS::CSScaleformImageCreator` | `0x2bbb3a0` | builds images from gfx imports; create-image = vt[1] `FUN_140d6bbc0` |
| `Scaleform::Render::TextureImage` | `0x2bb8838` | GFx texture-image base (carries rect/dims) |
| `CS::CSTextureImage` | `0x2bb8910` | ER subclass; ctor `FUN_140d68410` (rect fields, §2) |
| `CS::CSGxTexture` | `0x2b761b0` | CS wrapper over the GPU texture; ctor `FUN_140b7ce50` |
| `GXBS::IGXTexture` | `0x2b75fc8` | GXBS texture interface (shared base w/ SRV + refcount) |
| `GXBS::GXTexture2D` | `0x2f05928` | **concrete 2D texture → ID3D12Resource** (ctor `FUN_1419efa90`) |
| `GXBS::GXShaderResourceView` | `0x2b75fa0` | SRV base |
| `GXBS::IGXTextureRepository`/`CS::CSGxResourceRepository` | `0x2b6eac8` | holds all loaded GX textures (ctor `FUN_140b54260`) |
| `GXBS::GXCGTextureBuilder_TPF` | `0x2f0a228` | builds a GXTexture from a **TPF** (how the icon sheet loads) |
| `GXBS::GXRenderTarget2D` / `GXBS::GXRenderToTexture` | `0x2f05a60` / `0x2f248b0` | offscreen RT (Path B) |
| `CS::CSMovieGxTexture` | `0x2bded88` | a GFx **movie rendered into a GX texture** (Path B, §5) |

Render/display manager singleton = **`DAT_1447ef360`** (= the projection/resolution doc's
`er_base+0x47ef360`); `FUN_1419e7990(mgr, kind, …)` allocates a GX texture/output (used by the
movie-texture and dummy-texture ctors).

## 2. The sub-rect is C++-readable — `CS::CSTextureImage` ctor `FUN_140d68410`

```c
// FUN_140d68410(this, gxTexture, int rect[4], char useFullDims, u8 flip, u32, int dim[2], u32, ...)
this->vftable = CS::CSTextureImage::vftable;     // (Scaleform::Render::TextureImage subclass)
*(i32*)(this+0x2c) = dim[0];  *(i32*)(this+0x30) = dim[1];   // full sheet W,H
FUN_14011a360(this+0x38, gxTexture, …);                      // this+0x38 = backing CSGxTexture ptr
*(i32*)(this+0x74) = rect[0]; *(i32*)(this+0x3c) = rect[1];  // x0, y0
*(i32*)(this+0x7c) = rect[2]; *(i32*)(this+0x40) = rect[3];  // x1, y1   (px within the sheet)
// useFullDims==0 → rect = the int[4] sub-rect (a sprite cell); !=0 → rect = full texture
```
So a sprite cell's pixel rect within the sheet is right there on the image object — that is the
"iconId→rect" geometry the brief wanted, available without parsing the `.gfx`. Full dims at
`+0x2c/+0x30` give the normalization denominator (`u = x/Wsheet`, etc.).

## 3. The GPU texture — `GXBS::GXTexture2D` field layout (getters confirm)

From the vtable getters (`out_v70.txt`):
```
GXTexture2D + 0x40  ptr  → GXBS GPU-resource object (the ID3D12Resource wrapper)
                            desc lives at [+0x40]+0x20 (FUN_141e862f0 reads W/H/format/depth from it)
            + 0x48  ptr  → SRV / descriptor handle           (vt2 getter)
            + 0x50  ptr  → (double-deref target; resource view/heap)   (vt3)
            + 0x58  i32  → format / flag                       (vt13)
  vt6 GetWidth, vt7 GetHeight, vt8/vt9 GetDepth/Format, vt15 …  (all read [+0x40]+0x20)
  vt16 IsValid = ([+0x40] != 0)
```
`CSGxTexture` (ctor `FUN_140b7ce50`) is a thin CS wrapper sharing the GXBS base vtable slots
([11]=`FUN_140b7cca0`, [12]=`FUN_140b7cd20`, [14] common with GXTexture2D); it references the
concrete `GXTexture2D` (its `+0x10/+0x18`). At runtime just read the image's `+0x38` texture ptr and
treat it as an `IGXTexture` — call the `GXTexture2D` getters (resource @ `+0x40`) on it.

## 4. The iconId side — `FUN_140a82a80` build is NOT where the sprite is chosen

The point build `FUN_140a82a80` (already hooked for live-refresh) only walks the `+0x398` icon map
and dispatches into VMP thunks (`thunk_FUN_1426c7ff0`, `thunk_FUN_145c85788`); the instance ctor
`FUN_140a811e0` only inits the color/UV LUT (`FUN_1401899c0(…,0x77)`) and zeroes pos. **No iconId→
sprite selection in plain C++** — it happens Scaleform-side. The iconId itself is read via
`CS::WorldMapPointPinData::GetIconId` (RTTI string @ `0x2ad6704`), and the sprite is bound on the
`CS::WorldMapItemControl` GFx item (one of the 9 menu lists — see
`marker_affine_hook_re_findings.md` §"RESOLVED v61"). So the **iconId↔CSTextureImage** mapping is a
runtime capture, not a static table (§7).

## 5. Path A vs Path B

- **Path B (render the sprite to our own target)** is *natively* how ER makes movie textures:
  `CS::CSMovieGxTexture` (`FUN_140e1f5a0`) allocates a GX texture (`FUN_1419e7990(DAT_1447ef360,1,…)`)
  and a movie renders into it. But to render *one icon sprite* into *our* RT we'd have to drive the
  GFx movie/Scaleform renderer — across the VMP/Scaleform wall (the same wall that blocked
  `render_out` in `marker_affine_hook_re_findings.md`). High effort, fragile.
- **Path A (copy the D3D resource sub-rect)** needs only what we already have statically: the icon's
  `CSTextureImage` → rect (`+0x74/+0x7c/+0x3c/+0x40`, dims `+0x2c/+0x30`) + its `GXTexture2D` →
  `ID3D12Resource` (`+0x40`). One `CopyTextureRegion` per needed iconId. **Recommended.**

## 6. Readback / use recipe (Path A, with our captured DX12 handles)

Our handles (goblin_overlay.cpp): `g_device`, `g_command_queue`, `g_srv_heap`, `upload_rgba`.

For ImGui we need an SRV in our heap. Two variants:
1. **GPU→GPU (preferred, no CPU roundtrip, no de-BCn):** create our own `ID3D12Resource` (same
   DXGI format as the sheet, sized to the sub-rect, DEFAULT heap, `COPY_DEST`), record
   `CopyTextureRegion(dst, 0,0,0, src, &box)` with `box` = the rect, on a small command list on
   `g_command_queue`; transition our copy to `PIXEL_SHADER_RESOURCE`; create an SRV at a free
   `g_srv_heap` slot; hand that `gpu_handle` to `draw_marker` as the icon. ImGui samples a BCn SRV
   fine — no CPU decompress.
2. **CPU readback (only if you need RGBA on CPU):** copy into a `READBACK` buffer with
   row-pitch = `align(width·bpp, 256)`, fence-wait, `Map()`, de-pitch, and de-BCn if the format is
   `BC1/BC3/BC7` → then `upload_rgba`.

Barriers — the sheet is the engine's resource:
- Grab it during a **worldmap-open frame** when it is resident + in `PIXEL_SHADER_RESOURCE` (it's
  being sampled to draw native icons). Ideally **don't transition it**; `CopyTextureRegion` source
  can read from `PIXEL_SHADER_RESOURCE`-compatible state via a copy queue, or use
  `COMMON`/state-promotion. If you must transition, transition back to the exact prior state to avoid
  corrupting engine rendering. Do the copy on our own command list/allocator, never the engine's.

## 7. What is runtime-only (game not running during this RE)

1. **The iconId↔image binding + the actual resource pointer.** Resolve at runtime, two options:
   - **Hook `CS::CSScaleformImageCreator::CreateImage` (vt[1] `FUN_140d6bbc0`, RVA `0xd6bbc0`)**
     during worldmap-menu load: log each created `CSTextureImage` + its rect (`+0x74/...`) + backing
     `GXTexture2D` (`+0x38`→`+0x40`→resource). The worldmap icon sheet import shows up here. Map the
     images to iconIds by their import name / order.
   - Or **walk the live worldmap movie**: from the menu (`CS::WorldMapDialog`, `dialog = cursor−0x2DB0`)
     → its `WorldMapItemControl` lists → the bound Scaleform image (`CSTextureImage`). Heavier (GFx
     traversal) but no hook.
2. **Confirm one-sheet-with-rects vs many-images.** Read a few `CSTextureImage` rects vs their
   `GXTexture2D` pointer: if many images share one `GXTexture2D` with differing rects → single sheet
   (use the rects); if each iconId has its own `GXTexture2D` with full-image rect → per-icon textures
   (copy whole). Either way the §2/§3 fields cover it.
3. **Worked examples (deliverable):** with the map open, dump for iconId 370 (grace) and one of the
   missing six (e.g. 382 talisman): the `CSTextureImage` rect + sheet dims + the `GXTexture2D`
   resource desc (format/W/H). That yields the exact sub-rect to copy.

## 8. AOBs / handles

- bridge vtables: §1 table (resolve by RTTI/vtable scan — patch-robust).
- `CSTextureImage` ctor `FUN_140d68410` `0xd68410`; rect `+0x74/+0x7c/+0x3c/+0x40`, dims `+0x2c/+0x30`,
  backing tex `+0x38`.
- `GXTexture2D` ctor `FUN_1419efa90` `0x19efa90`; resource `+0x40` (desc `+0x20`), SRV `+0x48`,
  format `+0x58`; getters vt6/7/8/9/15 (`0x19f0780/0860/08e0/07f0/0710`), IsValid vt16 `0x19f0940`.
- `CSScaleformImageCreator::CreateImage` vt[1] `FUN_140d6bbc0` `0xd6bbc0` (hook point, §7).
- `CSMovieGxTexture` ctor `FUN_140e1f5a0` `0xe1f5a0`; alloc `FUN_1419e7990(DAT_1447ef360,…)`.
- display mgr `DAT_1447ef360` (`er_base+0x47ef360`); GX texture repo ctor `FUN_140b54260` `0xb54260`.
- iconId getter `CS::WorldMapPointPinData::GetIconId` (RTTI `0x2ad6704`); point build `FUN_140a82a80`.
- Offsets version-specific; pin classes by vtable scan, fns by AOB. The §2/§3 field contract + the
  CopyTextureRegion path are the stable part.
```
