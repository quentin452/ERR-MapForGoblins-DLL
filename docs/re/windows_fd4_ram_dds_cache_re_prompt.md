# RE brief — find the FD4 RAM copy of a decompressed menu texture (DDS bytes), so the overlay can upload icon sheets itself without the game's lazy GPU bind

App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Our DLL (MapForGoblins) is in-process on
Linux/Proton (vkd3d). Read-only RE. Deliverable: an offset recipe (ideally a Cheat Engine `.CT`)
from a known menu-texture name/handle to its **decompressed DDS/BCn bytes in CPU RAM**.

## Why

We want to draw real ER item icons on our overlay map markers without the player browsing each
item. Two known sources are both gated:

- **GPU sheet (the game's `ID3D12Resource`)** — only valid once the engine RENDERS the sprite
  (lazy GPU bind, findings §2). So harvest only covers what's been drawn (map open / item hovered).
  We already CopyTextureRegion from it when bound (`ensure_item_icon_srv`, `copy_er_sheet_direct`).
- **Disk file** `mod/menu/hi/01_common.tpf.dcx` — DCX-compressed (KRAK/oodle). Reading it ourselves
  needs an oodle decompress + TPF parse. Doable (lib `mod/menu/deploy/oo2core_6_win64.dll`) but the
  user wants to avoid re-decompressing.

**The hypothesis to verify:** the FD4 file/resource system decompresses each menu DCX→TPF→DDS at
load time and keeps the **decompressed DDS bytes in CPU RAM** (a resource cache), independent of
whether the texture is currently GPU-bound or which menu is open. If so, we can find those bytes by
name/handle and upload them into our OWN D3D12 texture (we already have the uploader,
`create_tex_from_dds_file` → split a `_mem` variant) — no lazy-bind wait, no re-decompress.

## What we already have (don't re-derive)

- **Find-by-name hook** `FUN_140d63c30` (`er+0xd63c30`), `void* find(void* repo, void** out, const wchar_t* key)`:
  resolves a `CS::CSTextureImage` for a `MENU_ItemIcon_<id>` / `MENU_MAP_*` key. `repo` stashed as
  `g_icon_repo`.
- From the `CSTextureImage img`: rect `img+0x74..0x80`; `img+0x10 → Render::Texture`, `+0x70 →
  ID3D12Resource` (the GPU sheet — **null/unbound until rendered**, that's the wall); vtable guard
  `img == er+0x2bb8910`.
- The sheet identity: dims + DXGI_FORMAT (`res+0x20/+0x28/+0x30`), e.g. 1024x2048 / 2048x1024 BC7.

## The questions

1. **Does a decompressed CPU-RAM copy of the DDS persist?** When a menu loads `01_common.tpf.dcx`,
   does FD4 keep the decompressed TPF (or per-texture DDS) in a heap buffer after upload, or free it
   once the GPU resource is created? Check around the TPF/texture load path and the FD4 resource
   repository (the same `repo` family the find hook uses).

2. **Reach the CPU bytes from a `CSTextureImage` / `Render::Texture`.** From `img+0x10` (Render::Texture)
   or its owning resource cap, is there a pointer to a host-memory blob (the DDS/BCn mip0 data) +
   size? Walk `Render::Texture` fields (and its HAL texture at `+0x70`-ish) for a CPU pointer that is
   NOT the `ID3D12Resource` (which is GPU-only). Look for a `{ptr, size}` pair sized ~= `blocksW*blocksH*blockBytes`.

3. **Or reach it via the FD4 file cache by name.** The DCX→TPF was loaded by path through the CSFile
   singleton (`er+0x3d5b0f8`, loader `FUN_1401f5560`). Is the decompressed TPF retained in a
   name→buffer map (FD4ResCap / FD4FileCap)? If so: name (e.g. the tpf/sheet name) → buffer + size →
   parse TPF → DDS. Find the cache container + its key/stride/value layout (same RB-tree style as the
   icon repo: node+0x00 left / +0x10 right / +0x19 nil, value at +0x50, per §8b).

## How to validate (runtime, like the player-pos / icon recipes)

- Open inventory once so a sheet is resident; grab a known `CSTextureImage`/sheet via the existing
  hook. Cheat-Engine-walk from it for a host pointer whose first 4 bytes are `"DDS "` (0x20534444)
  or a BCn block pattern, with a plausible size.
- Confirm it **survives** closing the menu (the whole point: bytes persist when the GPU sheet may
  unbind). Re-read after navigating away.
- Cross-check: the bytes at that pointer, fed to our `create_tex_from_dds`-style uploader, must
  produce the correct icon (compare against the GPU-harvested copy).

## Deliverable

A recipe: `CSTextureImage` (or sheet name) → **CPU DDS pointer + size**, validated to persist
menu-closed, plus the `.CT`. Then the overlay uploads it into our own texture (we own the SRV) and
crops any icon's rect — full coverage, no lazy-bind dependency, no re-decompress.

## If the hypothesis is FALSE

If FD4 frees the decompressed bytes after GPU upload (no persistent CPU copy), fall back to the
self-decompress path: read `01_common.tpf.dcx` → oodle `OodleLZ_Decompress` (oo2core_6_win64) → parse
TPF → DDS → our uploader. Record that finding so we stop chasing the RAM cache.
