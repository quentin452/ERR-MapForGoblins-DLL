# Map-tile loading (endgame phase-1a) — implementation plan

Goal: draw ELDEN RING's real world-map ART on the mod's virtual-map canvas (`panel_virtual_map`), so the
mod map looks like the game map — the first brick of the total-map-replacement endgame
(`virtual_world_multi_world_design.md`). Scoped 2026-07-04 from two recon passes. **This is a SLICE, not a
quick brick** — the whole DCX→DDS→GPU→draw chain exists, but getting the tile PIXELS has real work either way.

## Building blocks that EXIST (callable today)
- **DDS(BCn incl. BC7/DX10) → GPU SRV handle:** `create_tex_from_dds_mem(data,len,&w,&h,&fmt)` →
  `goblin_overlay.cpp:506`/exported `:2280`, decl `goblin_overlay_render.hpp:97`. Returns a `UINT64` GPU SRV
  handle usable directly as `ImTextureID`. **Caveats:** mip0 only; hard cap `g_next_item_srv>=256` (shared
  SRV heap, no recycle); must run OFF the per-frame icon batch (does its own cmd-list reset); needs
  g_device/g_command_queue/g_srv_heap.
- **Draw it:** `ImGui::Image((ImTextureID)handle,…)` or draw-list `AddImage/AddImageQuad` with the tile's
  screen quad from `w2s` (panel_virtual_map).
- **DCX decompress (DFLT+KRAK):** `msbe::dcx_decompress` (`msbe_parser.cpp:952`) + `resolve_oodle`
  (`oo2core_6_win64.dll` in-game / `liboo2corelinux64.so.9` offline).
- **Read a game file by rel-path, decompressed:** `read_game_file_decompressed(rel)` (`loot_disk.cpp:178`,
  loose→dvdbnd fallback) + `tpf_find_texture(buf,n,name,&off,&len)` (`msbe_parser.cpp:1002`). **Template =
  `read_item_icon_sheets` (`loot_disk.cpp:250`)** — the exact decompress→TPF→named-DDS→GPU chain, for
  `menu/hi/01_common.tpf.dcx`.
- **Install root:** `g_mod_folder`, `game_dir()` (parent of eldenring.exe), `resolve_map_dir()`
  (`loot_disk.cpp:32/60/84`). **This box's base game is VANILLA-PACKED** (`…/steamapps/common/ELDEN RING/
  Game/Data0-3.bhd` dvdbnd; NOT UXM-unpacked) — tiles live inside the encrypted dvdbnd, read via
  `dvdbnd::read_packed_file` (`dvdbnd_reader.{hpp,cpp}`).
- **RAM harvest resolver (per resident image):** in the repo+0xb0 twin walk (`harvest_twin_map_icons`,
  `goblin_icon_harvest.cpp:818`) each node gives `nm` (name) + `img`. From `img` (guard vtable =
  `er+0x2bb8910` CSTextureImage): rect `img+0x74/78/7c/80`, resource `img+0x10→rtex, rtex+0x70→res`, dims
  `res+0x20=W, +0x28=H, +0x30=fmt` (mirrors `cache_map_sprite_from_img` `:751`+). Whole-sheet copy exists:
  `copy_er_sheet_direct` (`goblin_overlay.cpp:585`).
- **Tile→world→canvas math:** `tileId = group*10000+gridX*100+gridZ`; `worldX = px + (gridX-gridXbase)*256`,
  `worldZ = pz + (gridZ-gridZbase)*256` (256 units/cell); overworld converter `scale 1, originX 7168,
  originZ 16384, bias 128/128`. Then the mod's `w2s` places the tile quad. Group 0/1/2 = separate coord
  spaces (pick the converter per group). (`windows_world_to_mapspace_projection_re_findings.md:70-100`.)

## The GAP = getting the tile pixels — two paths
### Path A — RAM harvest (resident textures; prime-directive preferred)
`force_load_file("menu:/71_MapTile.tpfbhd")` (`overlay_api::force_load_file`, wired at
`panel_dev_icons.cpp:73`) makes tiles resident as `MENU_MapTile_*` in the repo+0xb0 twin. Extend the twin
walk: for `MENU_MapTile_*` names, resolve resource+rect+dims (offsets above), store name→{res,rect,dims},
then `copy_er_sheet_direct`/`CopyTextureRegion` each into an SRV → draw at its grid quad.
- **Pro:** reuses the proven repo walk + img→resource + sheet-copy; mod-agnostic (active install's resident
  art); no archive-format work.
- **Con / WRINKLE:** `g_icon_repo` is captured by the find-hook only after a MENU/map draw once
  (`goblin_icon_harvest.cpp:480`), so residency/harvest may need a menu opened once (the same map-open
  dependency slice D hit). Confirm whether `force_load_file` alone populates the repo without a menu. SRV
  256-cap: the overworld is MANY tiles — need a bounded/streamed subset or a bigger/recycling SRV heap.
### ★ Archive format CRACKED (2026-07-04) — the inner-BHD5 gap is small
Peeked `mod/menu/hi/00_Solo.tpfbhd`/`.tpfbdt` (loose in ERR — the SAME `.tpfbhd` format as `71_MapTile`, a
free de-risk sample):
- **`.tpfbhd` magic `BHF4`** = an UNencrypted BHD5 split-archive HEADER. Header ~0x40 bytes (fileCount
  `@0x0C` = 0x121a here; version string "07D7R6" @0x18), then a fixed-stride ENTRY TABLE. Each entry ≈
  `{flags, nameHash, size:u64, paddedSize:u64, offset:u64 into the .tpfbdt}` (standard SoulsFormats **BXF4**
  layout — port that struct). Names are HASHES (FromSoft path hash), not strings.
- **`.tpfbdt` magic `BDF4`** = the DATA file. At each entry's offset sits a **`DCX\0`** blob (confirmed:
  first entry @0x30 = `44 43 58 00` "DCX").
- ⇒ Per tile = **BHF4 entry → seek .tpfbdt → `DCX` → TPF → DDS.** The `DCX→TPF→DDS` half is ALREADY DONE
  (`dcx_decompress` + `tpf_find_texture`). **The ONLY new code is the BHF4 entry-table parser** (find the
  entry whose nameHash matches the wanted tile, or iterate all entries). Small + well-understood.
- **Offline de-risk available NOW:** parse `00_Solo.tpfbhd`/`.tpfbdt` with a native-Linux tool (reuse
  `msbe::dcx_decompress` + `tpf_find_texture` + the Oodle `.so`, like `tools/menu_tex_extract.cpp`) to dump
  entry count + each inner TPF's DDS names + dims — zero game, zero DLL risk — before writing the DLL reader.

### Path B — Disk extract (in-game read; offline format-recon possible via 00_Solo)
`read_game_file_decompressed("menu/71_MapTile.tpfbhd")` (+ the `.tpfbdt`) → parse the **inner BHD5 split
archive** → each entry is a TPF (maybe DCX'd) → `tpf_find_texture` → DDS → `create_tex_from_dds_mem`.
- **Pro:** no residency dep; testable OFFLINE (write a standalone Linux tool first — read 71_MapTile out of
  the dvdbnd via `dvdbnd_reader`, dump entry names/format — to learn the structure before touching the DLL).
- **Con / GAP:** need an inner **BHD5 (tpfbhd/tpfbdt) reader** (dvdbnd_reader parses the top-level ENCRYPTED
  BHD5 — reuse its logic minus RSA) + the per-tile DDS naming inside. New code, light format RE.

## Recommended sub-slices (do Path B's offline recon FIRST to de-risk)
1. **Offline archive recon:** standalone tool — `dvdbnd::read_packed_file("menu/71_MapTile.tpfbhd"/".tpfbdt")`
   from the base-game dvdbnd, parse BHD5, dump entry count + names + first entry's magic (TPF? DCX? DDS?
   dims). Answers "what's inside + the naming" with zero game run, zero DLL risk.
2. **One tile → GPU → canvas:** whichever path recon-1 favours, load ONE tile, `create_tex_from_dds_mem`,
   draw it at its grid quad on the vmap canvas (Base ER world 0). Verify a real ER map fragment renders.
3. **All overworld tiles + positioning:** load group-0 tiles, place each by grid, mind the SRV cap (stream
   only visible tiles, or grow/recycle the SRV heap). The mod map now shows the ER overworld.
4. **Groups 1/2 (UG/DLC) + per-group converter coords.** Then fast-travel (separate feature) makes it a
   real map.

## Constraints to respect
- **SRV 256-cap + no recycle** — the biggest structural blocker for "all tiles". Plan a bounded/streamed
  set or extend the SRV heap + add a free list before loading a full map's worth of tiles.
- **create_tex_from_dds_mem must run off the icon batch** (own cmd-list reset) — call it from a controlled
  point, not mid-icon-draw.
- **mip0 only** — fine for a flat map view.
- Keep it mod-agnostic: read the ACTIVE install's tiles (Path A resident, or Path B dvdbnd/loose), never a
  baked snapshot.

## ★ In-game read CONFIRMED (2026-07-04, assets_probe)
`menu/71_MapTile.tpfbhd` resolves IN-GAME via `read_game_file_decompressed` = **packed(4.3MB)** from the base-game dvdbnd (the `assets_probe` RPC / `test_assets.py`). So Path B's dvdbnd read WORKS in-game (the Windows-only `dvdbnd_reader` caveat was only about OFFLINE Linux reads — the DLL under Proton reads it fine). Only the BHF4 entry-table parser + the DX12 SRV-cap streaming remain.
