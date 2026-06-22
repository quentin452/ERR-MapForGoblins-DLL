# RE follow-up — bind the worldmap icon CSTextureImage to its GPU texture + iconId

Follows `windows_runtime_icon_textures_re_findings.md`. We implemented the §7 runtime step
(hook `CSScaleformImageCreator::CreateImage` `FUN_140d6bbc0`) and **partially confirmed it on
App 2.6.2.0 / ERR 2.2.9.6 (runtime, read-only RPM)** — but two links don't hold at the hook
point. This brief asks for the exact runtime layout to finish Path A (CopyTextureRegion).

## What we CONFIRMED live (probe `dump_icon_textures`, [ICONTEX] log)
- `CreateImage` returns the **`CS::CSTextureImage`** — its own vtable RVA = **`0x2bb8910`** ✓
  (matches findings §1). Hook fires fine, no crash.
- **Sprite RECT offsets DIFFER from the findings**: the rect is **contiguous at
  `img+0x74`=x0, `+0x78`=y0, `+0x7c`=x1, `+0x80`=y1** (66×66 sprites), sheet dims at
  `+0x84/+0x88` (also at `+0x2c/+0x30`). The findings' `+0x3c/+0x40` for y0/y1 read 0/garbage.
  (Please re-confirm these rect offsets so we bake the right ones.)
- Multiple sheet sizes seen: mostly **512×512**, some **2048×1024**.

## What DOESN'T hold at the CreateImage hook point (the asks)
1. **The CSGxTexture / GXTexture2D is NOT linked from the image yet.** `CSTextureImage+0x38`
   (findings' "→ CSGxTexture") is a **static handle/string `er_image+0x1aa6840`** (same for every
   image), NOT a CSGxTexture. A bounded BFS over the CS object graph from the image (depth ≤4,
   300 nodes) found **NO** object with vtable `0x2f05928` (GXTexture2D) or `0x2b761b0`
   (CSGxTexture). So at CreateImage time the GPU texture appears **unbound / referenced by handle**.
   → **Where/when does the CSTextureImage get its concrete GXTexture2D (ID3D12Resource) bound?**
   The lazy bind site, or the lookup that resolves the `0x1aa6840` handle → the GX texture. If it's
   a repository lookup, give the key→texture path.
2. **The icon SHEET's ID3D12Resource, reachably at runtime.** Preferred: enumerate
   **`CS::CSGxResourceRepository`** (vtable `0x2b6eac8`, ctor `FUN_140b54260`) — its singleton/
   resolve chain + how to iterate its GX textures — so we find the worldmap icon sheet (the
   512×512 / 2048×1024 GXTexture2D) and read `GXTexture2D+0x40 → ID3D12Resource`. OR the
   **`GXCGTextureBuilder_TPF`** (`0x2f0a228`) hook point to capture the sheet GXTexture2D as the
   TPF loads. Give whichever is the stable runtime handle to the sheet resource.
3. **iconId ↔ CSTextureImage binding.** The `CreateImage` args we logged are NOT the import name
   (arg2 = a heap ptr whose bytes aren't an ASCII name; arg3 = a constant). The findings said the
   binding is on `CS::WorldMapItemControl` (one of the 9 menu lists) via
   `CS::WorldMapPointPinData::GetIconId` (RTTI `0x2ad6704`). → **The concrete runtime path
   iconId → the CSTextureImage** (which image holds iconId 370's sprite): the item-control field
   that points to the image, or a name/index we can read off the image (e.g. is `0x1aa6840` an
   atlas/region name we can parse, or is there an importId field on the image?).

## Deliverable
- Re-confirmed rect offsets (we measured `+0x74/+0x78/+0x7c/+0x80`) + sheet-dims offsets.
- The runtime chain (RVA/AOB + offsets) from a worldmap icon to its **GXTexture2D → ID3D12Resource**
  — via the image's lazy bind, or the CSGxResourceRepository enumeration, or a TPF-builder hook.
- The runtime path **iconId → CSTextureImage (rect)** so we can pull a specific icon (370 grace,
  382 talisman) — a worked example with the map open: iconId → image ptr → rect → sheet resource.

## Why
We have the rect + the image; the only gaps are (a) the live GPU-texture pointer for the sheet
and (b) the iconId↔image label. With those, Path A (CopyTextureRegion the sub-rect into our own
SRV, using our captured `g_device`/`g_command_queue`/`g_srv_heap`) gives us the 6 missing category
icons + the lit discovered-grace sprite, live, no FFDEC. The hook + RPM probe is already in place
(`config dump_icon_textures`, goblin_inject.cpp) to verify any offsets you return.
