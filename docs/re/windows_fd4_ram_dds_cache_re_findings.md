# RE findings — no persistent CPU DDS copy reachable from a CSTextureImage → take the self-decompress fallback

> **⚠ SUPERSEDED (2026-06-23).** The "no persistent CPU copy" conclusion is correct, but the chosen
> solution is BETTER than the self-decompress fallback below: instead of re-decompressing the
> `.tpf.dcx` from disk ourselves, the shipped pipeline **IAT-hooks ER's own `OodleLZ_Decompress`** and
> grabs the decompressed DDS from ER's RAM buffer *inside the hook* (the buffer is transient → copy it
> out there). Commits `d1553ac`/`8abd30b`/`9369bd3`/`8291c3a`/`0d501c3`. Texture pipeline is now solved
> (Oodle-hook → own texture manager → sheet-as-atlas; item iconId→rect via the find hook). Remaining
> gap (map-point icon iconId→rect) is `docs/re/windows_map_point_icon_layout_re_prompt.md`. Keep this
> doc only for §1/§2 (the CSTextureImage GPU-only struct map, still accurate).

Answers `docs/re/windows_fd4_ram_dds_cache_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
scripts `re_v136..v139`). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.

---

## 0. TL;DR

**The CSTextureImage → Render/HAL texture chain is entirely GPU-resident — there is no CPU-RAM DDS
pointer hanging off it.** Every field reachable from a `CSTextureImage` (the object the find-by-name
hook returns) is a GPU resource handle, dimensions, or a sub-rect. So brief **Q2 (reach the CPU bytes
from a CSTextureImage / Render::Texture) = NOT possible** — there is nothing to reach.

The decompressed DDS is uploaded to the GPU and the CPU staging buffer is not retained on these
objects. The only CPU-side bytes that could persist are a transient load buffer (freed) or, at most,
the **compressed** `.tpf.dcx` in the file subsystem (Q3) — which still needs an oodle decompress.

**Recommendation: take the brief's FALLBACK** — self-decompress `mod/menu/hi/01_common.tpf.dcx`
(oodle `OodleLZ_Decompress` via `oo2core_6_win64.dll`) → parse TPF → DDS → our existing uploader.
Deterministic, full coverage, no lazy-bind wait, no dependency on a RAM cache that isn't there.
(The brief's runtime CE-walk for a persistent `"DDS "` host pointer is the cheap 100%-confirm if you
want certainty before building the fallback — but the static chain below already says it won't find
one off a `CSTextureImage`.)

---

## 1. CSTextureImage is a GPU sub-image descriptor (no CPU blob) — vtable `0x2bb8910`

Field map (from the vtable getters + ctor `FUN_140d68410`):

| off | meaning |
|----|----|
| +0x00 | vtable (`CS::CSTextureImage`; also a `Scaleform::Render::TextureImage`) |
| +0x10 | ptr → the HAL/Render texture object (flags at `[+0x10]+0x44/+0x46`, getter `FUN_140d68880`) |
| +0x18 | ptr → a refcounted sub-object (released in `FUN_140d609c0`) |
| +0x28/+0x2c/+0x30/+0x34 | dims (full-sheet w/h etc.; getters `FUN_140d68860`/`vt[5] FUN_140d642a0`/`FUN_140d68940`) |
| +0x38 | ref (smart-ptr) to the source Render::Texture (`FUN_14011a360` assign in ctor) |
| +0x70 | byte: sub-image flag (0 = use full dims `+0x2c/+0x30`, 1 = use sub-rect `+0x84/+0x88`) |
| +0x74/+0x7c, +0x84/+0x88 | sub-rect origin + size (the icon's rect within the sheet) |

None of these is a host-memory DDS/BCn pointer. `+0x70` of the *Render::Texture* (per the brief) is the
`ID3D12Resource` — GPU only, null until the engine renders the sprite (the known lazy-bind wall).

## 2. The image factory copies GPU handles, not bytes — `FUN_140d650b0`

The CSTextureImage that the find-by-name returns is built by `FUN_140d650b0(dev, name, srcTex, desc…)`:
```c
hal = dev->vt[0xe0](dev, *(descObj+0x20)/*format*/, 0x80, …);   // make a HAL texture object
hal[0x80] = *(srcTex->vt[0x10]() + 0x10);                         // copy the GPU resource handle
img = FUN_140d68410(/*CSTextureImage ctor*/ …, hal, …);          // wrap it + rect/dims
Scaleform::RefCountImpl::Release(hal);
```
It takes an **already-created** source texture and makes a sub-image **view** over the same GPU
resource. No DDS bytes are read here (they were consumed upstream at sheet upload), and nothing CPU-side
is stored. `hal+0x80` = the GPU resource; `hal+0x38/+0x3c` = dims. (`CSFile` "loader"
`FUN_1401f5560` is just `operator_new(0x98)` — an allocator stub, not the data path.)

## 3. Why the magic-scans found nothing (and what it means)

The `"DDS "` dword `0x20534444` (6 data occurrences, **0 code refs**) and the `TPF\0` dword
`0x00465054` (**0 occurrences**) are not compared as immediates anywhere — the DDS/TPF parse lives in
the resource/oodle layer and reads headers field-wise, not via a literal compare. So there's no quick
static hook onto "the DDS parser"; consistent with the bytes being handled transiently in the loader
and not surfaced on the durable texture objects.

## 4. Recommendation (the fallback, fully viable)

Self-decompress, since it's deterministic and we already own the uploader:
1. Read `mod/menu/hi/01_common.tpf.dcx` (and siblings) from disk.
2. DCX header (`DCX\0` / KRAK) → `OodleLZ_Decompress` via `mod/menu/deploy/oo2core_6_win64.dll`
   (`LoadLibrary` + `GetProcAddress("OodleLZ_Decompress")`). Output size is in the DCX header.
3. Parse the TPF (header + per-entry: name, offset, size, format) → each entry is a DDS.
4. Feed the DDS to the overlay's existing `create_tex_from_dds_file`-style uploader (split a `_mem`
   variant that takes a byte span instead of a path). We own the SRV → crop any icon rect by the
   `CSTextureImage` rect (`+0x74/+0x7c/+0x84/+0x88`) we already read by name.

Result: full icon coverage with no lazy-bind dependency and no re-decompress at runtime beyond the
one-time load. Keep the GPU-harvest path (`ensure_item_icon_srv` / `copy_er_sheet_direct`) as the
already-bound fast path; the self-decompress fills the gaps.

## 5. Handles / RVAs
- find-by-name family `FUN_140d63c30` `0xd63c30`; icon repo `g_icon_repo` (RB-tree, value=CSTextureImage).
- CSTextureImage vtable `0x2bb8910`; ctor `FUN_140d68410` `0xd68410` (sub-image) / `FUN_140d68550`/`FUN_140d68690`; getters `FUN_140d68860/68940/642a0/688a0/68880`.
- image factory `FUN_140d650b0` `0xd650b0` (HAL create via `dev->vt[0xe0]`; GPU handle → `hal+0x80`).
- Render/HAL texture: GPU `ID3D12Resource` at `+0x70` (CSTexImg's Render::Texture) / `hal+0x80` (factory) — GPU only.
- DCX/oodle: `mod/menu/deploy/oo2core_6_win64.dll` `OodleLZ_Decompress`; sheets `mod/menu/hi/01_common.tpf.dcx`.
- NOT FOUND (so don't chase): a CPU `{ptr,size}` DDS blob on any name-reachable texture object.
```
