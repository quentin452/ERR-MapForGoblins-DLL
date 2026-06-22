# RE follow-up findings — bind the icon CSTextureImage to its GPU texture + iconId

Answers `docs/re/windows_runtime_icon_textures_followup_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v75..v77`) + the live `[ICONTEX]` probe the user shipped.
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only. Corrects two offsets in
`windows_runtime_icon_textures_re_findings.md`.

---

## 0. TL;DR — the BFS found nothing because the texture is bound LAZILY at render, one hop deeper

The image never stores a `GXTexture2D` pointer. It stores a **`Scaleform::Render::Texture` at
`CSTextureImage+0x10`**, populated **lazily on first render** (GetTexture). At the `CreateImage`
hook (load time) `+0x10` is unbound → the probe correctly saw nothing. And even when bound, `+0x10`
is a *Render::Texture*, not a `GXTexture2D` — the `GXTexture2D` is one further hop (the probe's BFS
neither waited for render nor included the Render::Texture vtable).

- **Ask #1 (lazy bind)** → `GetTexture = CSTextureImage vt[+0xA8]` (`FUN_140d607f0`); it caches the
  Render::Texture at **`img+0x10`** (creating it from the GFx renderer `DAT_144593250` on first
  use). **Read `img+0x10` on a MAP-OPEN frame**, then hop to the `GXTexture2D` → `+0x40`
  ID3D12Resource. §2.
- **Ask #2 (sheet resource)** → the same `img+0x10 → GXTexture2D+0x40` chain (read at render) is the
  most direct; the TPF-builder / `CSGxResourceRepository` routes also work but are heavier. §3.
- **Ask #3 (iconId↔image)** → **no flat C++ map**; the iconId→sprite lives in the GFx movie
  (Scaleform/VMP). Label rects by the image's **import name** (logged in the CreateImage hook) or by
  a one-time **runtime correlation** (hover a known on-screen icon → its rect). §4.
- **Rect offsets** → the user's live read is correct; my original `+0x3c/+0x40` for y was wrong. §1.

## 1. Corrected rect / dims offsets (confirms the live `[ICONTEX]` measurement)

```
CSTextureImage (vtable RVA 0x2bb8910):
  +0x10  ptr   → Scaleform::Render::Texture   (LAZY — null until first render; see §2)
  +0x74  i32   rect x0      ┐ contiguous sprite sub-rect (px in the sheet)
  +0x78  i32   rect y0      │   (the findings' +0x3c/+0x40 for y were WRONG — read 0/garbage)
  +0x7c  i32   rect x1      │
  +0x80  i32   rect y1      ┘
  +0x84  i32   sheet W      ┐ also mirrored at +0x2c/+0x30
  +0x88  i32   sheet H      ┘
```
Live: 66×66 sprites; sheets mostly 512×512, some 2048×1024. (Patch the original findings §1 to this.)

## 2. The lazy bind — `GetTexture` (deliverable #1)

`CSTextureImage::GetTexture` = **vt[+0xA8]** = `FUN_140d607f0` (RVA `0xd607f0`):
```c
void* GetTexture(CSTextureImage* this, void* renderer) {
    if (this->[+0x10] && *(int*)(this->[+0x10] + 0x40) == 5)   // source present but unresolved (type 5)
        return FUN_141147700(renderer);                        // create/resolve via the renderer
    return this->[+0x10];                                      // the cached Render::Texture
}
```
- **`img+0x10` is the bound `Scaleform::Render::Texture`.** It is set lazily: 0/unresolved at
  `CreateImage`, populated the first time the image is drawn (or when `GetTexture` is called). The
  render methods vt22/vt23 (`FUN_1411482b0`/`FUN_1411483c0`) call `img->vt[+0xA8]` then the returned
  texture's `vt[0x38]` (texture matrix) and read its size at `tex+0x3c` — i.e. the engine itself
  only obtains the GPU texture through this call, never a flat field.
- The renderer/TextureManager is the singleton **`DAT_144593250`** (used in the texture-matrix
  methods `FUN_141148030`/`FUN_1411481d0` as `(*DAT_144593250)+0x78`).

**So the probe fix is simple:** don't read at the `CreateImage` hook (load) — read **on a frame where
the world map is open and icons have drawn**. Then:
```
renderTex = *(void**)(img + 0x10);        // bound now (non-null)
// renderTex is a Scaleform Render::Texture wrapping the GX texture — hop to the GXTexture2D:
//   walk renderTex's ptr fields for vtable 0x2f05928 (GXTexture2D) / 0x2b761b0 (CSGxTexture),
//   OR call img->vt[+0xA8](DAT_144593250) to force-resolve and return it.
gxTex = <that object>;                    // GXBS::GXTexture2D
res   = *(void**)(gxTex + 0x40);          // ID3D12Resource (desc at +0x20; vt6/7/8 = W/H/format)
```
(The user's BFS already had the GXTexture2D/CSGxTexture vtable targets — it just ran at the wrong
time and stopped one hop short. Re-run it rooted at `*(img+0x10)` during a map-open frame, or add the
`img+0x10` deref + include the Render::Texture as a transit node.)

If you'd rather not depend on render order, **call `GetTexture` yourself** on the render thread (the
Present/ExecuteCommandLists hook): `img->vt[+0xA8](DAT_144593250)` returns the Render::Texture, which
force-resolves the GPU texture. Don't call it off-thread (Scaleform renderer is render-thread-bound).

## 3. The sheet ID3D12Resource (deliverable #2)

Most direct = the §2 chain: `img+0x10 → Render::Texture → GXTexture2D+0x40 → ID3D12Resource`,
read on a map-open frame. `GXTexture2D` (vtable `0x2f05928`) getters (v70): resource `+0x40`
(desc `[+0x40]+0x20`), SRV `+0x48`, format `+0x58`, `IsValid` = `+0x40!=0`.

Alternatives (heavier, use only if the per-image hop proves flaky):
- **`GXCGTextureBuilder_TPF`** (vtable `0x2f0a228`): hook the TPF-load path to capture each sheet
  `GXTexture2D` + name as it loads when the menu opens. (vt[1] `FUN_141a009a0` is a mip/format config
  step, not the whole build — find the build entry via its caller if you take this route.)
- **`CS::CSGxResourceRepository`** (vtable `0x2b6eac8`, ctor `FUN_140b54260`): the GX-texture repo;
  enumerate its textures and match the icon sheet by dims (512×512 / 2048×1024). More plumbing than
  the per-image hop.

## 4. iconId ↔ CSTextureImage (deliverable #3) — runtime correlation, no static map

There is **no flat C++ `iconId → CSTextureImage` table**. The iconId→sprite selection happens in the
GFx movie (the worldmap `.gfx` sprite sheet), behind Scaleform/VMP — the same wall as the projection
cross-fade and `render_out`. Two practical labels:

1. **The image import NAME.** `CreateImage` builds the image from an import descriptor (`param_3`); the
   resolve path `FUN_140d64490` formats a key `L"%s_ptl"` from it. Log that **name string** in the
   existing CreateImage hook (follow `param_3` to its DLString, not the raw arg the probe logged as
   non-ASCII). If the worldmap icon import name encodes the sheet/cell, that is the label; map it to
   iconId once, offline.
2. **On-screen correlation.** With the map open, hover a **known** icon (e.g. a grace = iconId 370) and
   match its drawn sprite to the `CSTextureImage` whose rect the engine uses — gives iconId→rect for
   that icon. Repeat for the 6 missing categories. A handful of exact correlations beats a static map
   that doesn't exist.

Either way, the **rect + sheet** (which you already read) is enough to *copy* the sprite; the iconId is
just the label, and labeling is a one-time runtime/offline step.

## 5. Net path for Path A (CopyTextureRegion)

1. Keep the `CreateImage` hook to enumerate worldmap images + their rects/dims + import name (§1/§4).
2. On a **map-open frame**, for each needed image read `img+0x10 → GXTexture2D+0x40 = ID3D12Resource`
   (§2), or force it via `GetTexture(img, DAT_144593250)` on the render thread.
3. `CopyTextureRegion` the rect (`+0x74/+0x78/+0x7c/+0x80`) from that resource into our own SRV
   texture (GPU→GPU; `g_device`/`g_command_queue`/`g_srv_heap`) — ImGui samples BCn fine.
4. Label by iconId via §4 (import name or one hover-correlation per category). Keep the baked PNG as
   the cold-start fallback.

## 6. Handles

- GetTexture `CSTextureImage vt[+0xA8]` = `FUN_140d607f0` `0xd607f0`; bound Render::Texture at
  `img+0x10`. renderer/texmgr singleton `DAT_144593250`. resolve-create `FUN_141147700`.
- rect `+0x74/+0x78/+0x7c/+0x80`, dims `+0x84/+0x88` (and `+0x2c/+0x30`). image vtable `0x2bb8910`.
- `GXTexture2D` vtable `0x2f05928`: resource `+0x40` (desc `+0x20`), SRV `+0x48`, format `+0x58`.
  `CSGxTexture` `0x2b761b0`. CreateImage `FUN_140d6bbc0` `0xd6bbc0`; resolve `FUN_140d64490` `0xd64490`.
- sheet alt routes: `GXCGTextureBuilder_TPF` `0x2f0a228`; `CSGxResourceRepository` `0x2b6eac8` (ctor
  `FUN_140b54260`). Resolve by vtable scan / AOB; offsets version-specific.
- Runtime confirm: extend the `[ICONTEX]` probe to also read `img+0x10` (and its `+0x40` hop) on a
  map-OPEN tick (not just at CreateImage); expect non-null → GXTexture2D → a valid ID3D12Resource of
  the 512×512 / 2048×1024 sheet.
```
