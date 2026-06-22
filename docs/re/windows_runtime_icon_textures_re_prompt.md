# RE brief — recover world-map ICON textures at RUNTIME (kill the baked FFDEC atlas)

**Goal:** read the game's OWN world-map icon textures live (from the loaded DX12 / Scaleform
resources) and use them as the overlay's marker icons, instead of our BAKED PNG atlas. Same
philosophy as the just-landed live world→map projection (we call the engine instead of baking):
read live → auto-matches ERR / any mod's actual icons, gets the icons we DON'T have, and drops
the FFDEC pipeline entirely.

App 2.6.2.0 / ERR 2.2.9.6, our DLL is in-process with a working DX12 Present hook. Resolve as
`[er_base+RVA]+offsets` / AOBs, runtime-verified — same style as the SOLVED projection
(`windows_world_to_mapspace_projection_re_findings.md`) and player-pos
(`windows_player_pos_RESOLVED_re_findings.md`).

## Why this matters (what's broken with baking)
- **6 categories have NO icon** → they draw as a coloured circle fallback (by design today, but
  ugly): EquipTalismans (iconId 382), KeyWhetblades (431), LootThrowables (409), LootUtilities
  (412), WorldMaps (406), WorldQuestNPC (443). The atlas simply lacks these cells.
- **We lack the DISCOVERED/lit grace sprite** (rays/glow). Our atlas iconId 370 = the INERT
  (undiscovered) grace only. So we can't fully replace native graces (see the native-pin
  suppression plan — it has to KEEP native graces precisely because we can't draw the lit one).
- **Regen needs FFDEC** (`ffdec-cli.jar`, not installed on the Linux build box; `assets/map_icons/
  by_iconId/` is empty) → adding/refreshing an icon is blocked.
- **Baked = per-mod drift.** ERR (and icon mods) reskin map icons; our baked PNG is vanilla-ish.
  Live textures always match the loaded regulation/assets.

## What we do today (baked, to be replaced)
`src/generated_shared/goblin_overlay_icons.cpp` = `ATLAS_PNG[]` (a ~135 KB PNG blob, 40px cells)
+ `ICON_CELLS` (key→col,row). At load, `try_upload_atlas` (goblin_overlay.cpp) decodes it with
stb_image → RGBA → `upload_rgba()` → an `ID3D12Resource` + SRV at heap index 1; `icon_uv`
(map_renderer.cpp) computes each category's UV from its cell. One icon PER CATEGORY (not per item).

Native icons are drawn by the worldmap Scaleform movie (`assets/menu/02_120_worldmap_vanilla.gfx`)
by `iconId` (370–443 seen) — a full sprite sheet. That's why we pre-render-to-PNG today (Flash/
Scaleform can't be blitted directly). **This brief is about getting those sprites from the live
GPU/Scaleform resource instead.**

## Our DX12 handles (already captured in goblin_overlay.cpp — use these)
- `ID3D12Device* g_device`
- `ID3D12CommandQueue* g_command_queue`  (captured from the ExecuteCommandLists hook)
- `ID3D12DescriptorHeap* g_srv_heap`  ([0]=ImGui font, [1]=our atlas; we can add more SRV slots)
- `upload_rgba(rgba,w,h,srv_index,&tex,&gpu)` — the existing create-texture+SRV path; a readback
  is the inverse (copy a game resource → readback heap → map → RGBA).

## What we NEED (decompile + runtime-confirm)
The crux is two unknowns; either path (A texture-resource, or B Scaleform) is acceptable.

1. **The icon ATLAS texture resource.** Which `ID3D12Resource` holds the worldmap icon sprite
   sheet (the TPF the gfx loads), and how to reach its pointer at runtime (a resolvable chain:
   CSWorldMap menu / the Scaleform movie object → its texture(s) / a GFx `Texture`→`D3DTexture`).
   Report the resolve chain (+AOB/RVA), the resource's DXGI format + dimensions, and whether it's
   one sheet for all iconIds or several.
2. **The per-iconId frame RECT** (u0,v0,u1,v1 or px x,y,w,h) inside that sheet — the sprite-sheet
   layout keyed by iconId. This is the geometry we can't guess (same role the LegacyConv table
   played for projection). Where does the game store iconId→rect? Likely the gfx's sprite/frame
   table, or a `MENU_*` layout param, or the WorldMapPointIns build that selects the sprite for an
   iconId. Give the lookup (param id / table / fn) + a worked example (iconId 370 grace → its rect).
3. **A readback recipe.** Given the resource + a rect, the safe in-process copy: a `CopyTextureRegion`
   from the (possibly DEFAULT-heap, non-mappable) game resource into a READBACK `ID3D12Resource`,
   on `g_command_queue` with a fence wait, then Map() → RGBA (handle row-pitch alignment + the DXGI
   format / BC compression → decompress if BCn). Note any state-transition / barrier needed on the
   game resource (it's the engine's — don't corrupt its state; ideally copy when it's in
   PIXEL_SHADER_RESOURCE during the worldmap frame).

## Leads (pre-loaded so the agent skips dead ends)
- Native icon BUILD = `FUN_140a82a80` (AOB `WORLDMAP_POINT_CTOR`, we already hook it) sets the
  WorldMapPointIns `iconId`. The Scaleform side then maps iconId → sprite. Follow from the iconId
  write to where the movie selects the frame.
- `CSWorldMapPointMan` static `[er+0x3D6E9B0]+0x398` (the icon std::map) — the icon instances carry
  the iconId; the texture is shared by the menu/movie, not per-instance.
- The gfx movie `02_120_worldmap` is a `GFx::MovieDef`/`Movie` under the menu system; its imported
  textures become DX12 resources via the ER Scaleform→D3D bridge. Find that bridge (GFx
  `TextureManager` → `D3DTexture`/`ID3D12Resource`).
- We have the Present/ExecuteCommandLists hooks (goblin_overlay.cpp) → a frame where the worldmap is
  open is the moment to grab the resource (it's resident + bound).

## Deliverables
- The icon-sheet resource resolve chain (RVA/AOB + offsets) + its format/size, runtime-confirmed by
  reading it while the world map is open.
- The iconId→rect table/lookup + a worked example for iconId 370 (grace) and one of the 6 missing
  ones (e.g. 382 talisman) → the exact sub-rect.
- A C++ readback snippet using our `g_device`/`g_command_queue` that yields RGBA for a given iconId
  (CopyTextureRegion → readback → map → de-pitch → de-BCn if needed).
- Confirmation of which approach (A: copy from the D3D resource; B: ask Scaleform to render the
  sprite to an offscreen target we own) is more robust given VMProtect / engine ownership.

## Plan once answered
Add SRV slots for the recovered icons; on first worldmap-open, read each needed iconId's sub-rect
→ upload as its own small texture (or composite a runtime atlas) → point `icon_uv` / `draw_marker`
at the live texture for the 6 missing categories + the discovered grace + any reskinned icon; keep
the baked PNG as the cold-start fallback (before the resource resolves), exactly like
`live_projection` falls back to the baked affine. Then the FFDEC pipeline can be retired.

## Why
Mirrors the live-projection win: replace a baked, drift-prone, tool-gated asset with the engine's
own live data. Gets the icons we structurally cannot bake (missing 6, lit grace), auto-tracks ERR /
icon mods, and removes the FFDEC dependency. The DX12 hook makes the readback purely a
find-the-resource-and-rect problem — which is what this brief is.
