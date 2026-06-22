# RE findings — runtime item-icon SPRITE resolution (mod-robust, no FFDEC)

Answers `docs/re/windows_runtime_item_sprite_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
scripts `re_v88..v90`). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only. Builds on
`windows_menu_item_icon_re_findings.md` + `windows_runtime_icon_textures_followup_re_findings.md`.

---

## 0. TL;DR

There IS a **draw-free symbol→image lookup** — the icon image is found **by name** in the loaded FD4
image repository, so any icon on a **loaded sheet** is resolvable without the menu drawing it (this
gets past the "CreateImage only fires for drawn icons" wall). The genuine limit is **residency**: the
item-icon sheet gfx/TPF is loaded with the inventory menu and likely evicted when it closes, so for
world-overlay loot markers you must keep it resident or capture while the menu is open.

- **NEW — icons are grouped into SHEETS of 1000.** Two exe format families: per-icon
  `MENU_ItemIcon_%05d` (table base `0x3b34c28`) and **per-sheet** `MENU_ItemIcon_%02d000d` fed
  `iconId/1000` (table `0x3b34c40`, via `FUN_14073d5a0`). ⇒ **sheet = `iconId/1000`, cell =
  `iconId%1000`.** ERR names per-icon symbols `MENU_FL_<id>`.
- **Ask #1 — symbol→image (no draw):** `FUN_140d63c30(DAT_143d82510, &out, L"MENU_ItemIcon_<id>")`
  (and twin `FUN_140d63e50`) = **find image resource by name** in the FD4 image repo `DAT_143d82510`;
  returns the `CS::CSTextureImage` (rect `+0x74/+0x78/+0x7c/+0x80`) or `0` on miss. The widget
  `FUN_14074bcc0` uses exactly this (per-icon key → miss → sheet key fallback). The loaded movie's
  image list is also enumerable: `FUN_140d69640(movie)` walks `[movie+0x40]+0x90`, entry type
  `+0x88==4` = image. §1.
- **Ask #2 — draw-free sheet `ID3D12Resource`:** from the found image, the solved chain
  `img+0x10 → Render::Texture+0x70 → ID3D12Resource` (no draw needed once the image is resolved). §2.
- **Ask #3 — force-create fallback:** if the per-icon image isn't resident, the engine create path is
  `CSScaleformImageCreator::CreateImage` (`FUN_140d6bbc0`) — needs the menu's `GFxMovieView` context;
  the find-by-name (#1) is cleaner when the sheet is loaded. §3.
- **Ask #4 — mod-robust:** the symbol→sprite resolution + rects come from the **loaded gfx**
  (ERR's `MENU_FL_<id>`), not an exe table. Only the format *strings* are exe-baked. Confirmed. §4.

---

## 1. The draw-free symbol→image lookup (ask #1)

The icon-image resolution is a **find-by-name** in the FD4 image repository singleton
**`DAT_143d82510`**:
```
key  = L"MENU_ItemIcon_%05d" % iconId          (FUN_14073d9d0, per-icon; ERR gfx symbol = MENU_FL_<id>)
img  = FUN_140d63c30(DAT_143d82510, &out, key)  // RVA 0xd63c30 (twin FUN_140d63e50 0xd63e50)
   → CS::CSTextureImage* (vtable 0x2bb8910) or 0 on miss
   rect = img+0x74/+0x78/+0x7c/+0x80 (x0,y0,x1,y1) ; sheet dims img+0x84/+0x88
```
The widget `FUN_14074bcc0` (`0x74bcc0`) does this, then on miss builds the **sheet key**
`MENU_ItemIcon_<iconId/1000>000d` (`FUN_14073d5a0` `0x73d5a0`, table `0x3b34c40`) and looks that up,
indexing the sheet by the intra-sheet frame (`iconId%1000`). So:
- **If the per-icon image is loaded** → `FUN_140d63c30` returns it (rect ready), no draw.
- **If only the sheet is loaded** → resolve the sheet image, frame = `iconId%1000`.

Either way this is a **name lookup over LOADED resources**, not a per-draw event — so it reaches any
icon whose sheet is resident, not just drawn ones. (Note: the exact find return is partly obscured by
a `noreturn`-misflagged string helper `FUN_14011b190` in the decompile; the call shape + the widget's
null-check/fallback establish it as find-returns-0-on-miss. Runtime-confirm by calling it for a known
loaded icon — see §6.)

**Enumerate without any key (the movie resource table, #1's "walk"):** `FUN_140d69640(movie)` iterates
the loaded movie's resource list at `[movie+0x40]+0x90`, picking entries whose type byte `+0x88==4`
(image), and resolves each via `FUN_140d63c30`. Caller `FUN_140d790a0` (`0xd790a0`) supplies the
movie. So the loaded inventory movie's image resources can be walked to enumerate every resident
icon's `CSTextureImage`+rect.

## 2. Draw-free sheet `ID3D12Resource` (ask #2)

Once you hold the `CSTextureImage` from §1, the GPU resource is the already-solved chain (no draw):
`img+0x10 → Scaleform Render::Texture (vt 0x2c0f318) → +0x70 → ID3D12Resource` (`+0x10` DIM==3,
`+0x20` W, `+0x28` H). Sheets dedup by resource ptr (a few atlases). `CopyTextureRegion` the §1 rect
from that resource into our SRV (`g_device`/`g_command_queue`/`g_srv_heap`), GPU→GPU. (`img+0x10` is
populated when the image is resolved/bound; if a pure name-find leaves it null until first sample,
fall back to §3 or to the sheet image which is bound.)

## 3. Force-create fallback (ask #3)

If neither the per-icon image nor its sheet is resident, the create path is
`CSScaleformImageCreator::CreateImage` (`FUN_140d6bbc0`, AOB `WORLDMAP_CREATE_IMAGE`) — it builds the
image from an import descriptor + the repo. It needs a live `GFxMovieView` (the inventory menu loaded)
and runs in the Scaleform/render context, so calling it for arbitrary ids off the draw path is
fragile. Prefer §1 (find) when the sheet is loaded; use create only as a last resort with the menu up.

## 4. Mod-robustness (ask #4) — loaded-gfx, not exe-baked

The **format strings** (`MENU_ItemIcon_%05d`, the `0x3b34c28`/`0x3b34c40` tables) and the
`iconId/1000` sheet math are exe-baked. **Everything that maps a symbol to a sprite + its rect is in
the loaded `.gfx`** (the repo `DAT_143d82510` holds the loaded gfx's image resources; ERR's symbols
are `MENU_FL_<id>`, captured live). So the §1/§2 path is **mod-correct by construction** — it reads
whatever atlas the active regulation/gfx ships. Nothing about the rect is in the exe.

## 5. The residency limit (the real caveat)

`FUN_140d63c30` finds only **loaded** resources. The item-icon sheet gfx/TPF is loaded with the
**inventory/equipment menu** and is most likely **evicted when the menu closes**. World-overlay loot
markers are drawn with the menu CLOSED → the sheets may not be resident → find fails. Practical
options, cheapest first:
1. **Capture-while-open:** when the inventory is open, walk the movie resources (§1 `FUN_140d69640`)
   or resolve each needed iconId, `CopyTextureRegion` its rect into **our own** persistent atlas
   (SRV), and draw from that with the menu closed. One pass per session caches all seen items.
2. **Keep-resident:** hold a ref on the sheet image/resource so it isn't evicted (riskier; engine
   owns lifetimes).
3. **Force-load** the menu gfx (heaviest; needs the create/movie path, §3).
Cold fallback = the baked per-category PNG (mirrors `live_projection`'s baked fallback).

## 6. Handles + runtime-confirm

- find-by-name `FUN_140d63c30` `0xd63c30` (twin `FUN_140d63e50` `0xd63e50`); repo singleton
  `DAT_143d82510`. per-icon key `FUN_14073d9d0` `0x73d9d0` (table `0x3b34c28`); **sheet key**
  `FUN_14073d5a0` `0x73d5a0` (table `0x3b34c40`, arg `iconId/1000`). widget `FUN_14074bcc0` `0x74bcc0`.
- movie resource walk `FUN_140d69640` `0xd69640` (list `[movie+0x40]+0x90`, type `+0x88==4`), caller
  `FUN_140d790a0` `0xd790a0`. create path `FUN_140d6bbc0` `0xd6bbc0`.
- image: `CSTextureImage` vt `0x2bb8910`, rect `+0x74/+0x78/+0x7c/+0x80`, dims `+0x84/+0x88`, name
  `+0x40`, `+0x10 → Render::Texture (0x2c0f318) +0x70 → ID3D12Resource`.
- sheet/cell: `sheet = iconId/1000`, `cell = iconId%1000`.
- **Runtime-confirm:** with the inventory OPEN, call `FUN_140d63c30(*er+0x3d82510, &out,
  L"MENU_ItemIcon_<id>")` for a NON-displayed item's iconId → expect a non-null `CSTextureImage`
  whose rect matches your `[ICONMAP]` capture for that id (proves draw-free find). Then close the
  menu and retry → confirms/measures the residency limit (§5).
```
