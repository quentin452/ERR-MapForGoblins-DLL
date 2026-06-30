# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.
Last updated: 2026-06-30.

## Active branch
`feat/native-poi-icons` (off `feat/dvdbnd-packed-reader` off `master`). NOT pushed. Build+deploy on Linux
run **sandbox-disabled**; deploy target `<ERR_ROOT>/dll/offline/MapForGoblins.dll` (ERR_ROOT=
`/home/iamacat/Games/ERRv2.2.9.6`). Verify deployed md5 == build output after every deploy.

## Prime directive (see AGENTS.md → Design principles)
Mod-agnostic first. Read icons/glyphs/markers/params from the ACTIVE install's real files (resident or
disk via Oodle/dvdbnd). Baked atlas = ERR-frozen artifact → eliminate. Circle = universal fallback.

## DONE this arc (committed)
- Map-open device-removed crash fixed (submit_and_wait clears g_icon_batch_open). `0df36a3`
- Item-icon layout-load hoisted off the res_tick path → loads on map-open without inventory. `e4fc128`
- Layout parser: forward-track imagePath (fixed 16KB back-scan that silently baked late-atlas entries). `d572ffc`
- Mod-agnostic prime directive added to AGENTS.md. `b8249f9`
- Offline menu texture extractor `tools/menu_tex_extract` (Linux Oodle works). `fc756c3`
- `parse_map_point_layout` scaffold → g_map_point_layout / g_map_point_named (not yet consumed). `31025ad`
- Map-point glyph IDs confirmed by eye → `docs/memory/features/map-point-glyph-ids.md`. `5cf441a`,`78b0b4b`

## IN PROGRESS
- **Bonus-1: undiscovered-grace icon.** Fork building the disk map-point render path (`map_point_glyph_uv`
  helper + accessors) and wiring undiscovered graces to draw `MENU_MAP_Player_02` (gold figurine, rect
  286,956,42,62 on SB_MapCursor) from disk; discovered = bonfire+check unchanged. AWAITING fork result +
  in-game test (look for `[GRACEUNDISC]` log; undiscovered graces should show the figurine).

## QUEUE (ordered)
1. **Bonus-2: SummoningPools → native glyph.** Wire `category_gpu_iconId(WorldSummoningPools)` to
   `MENU_MAP_89` (Martyr Effigy, SB_MapCursor_02) — OR `MENU_MAP_21` (2 figures, SB_MapCursor). DECISION
   PENDING: which glyph. Uses the same disk path as bonus-1.
2. **Bonus-3: quest NPC → `MENU_MAP_80`.** Belongs on the `feature/quests` branch, not here. Defer.
3. **Per-item icons (interactive map).** Draw each loot marker's ACTUAL item iconId (not the category
   rep) via the existing native_item_icon path. VERIFIED FEASIBLE + ~free VRAM: item icons are atlased
   into ~14 shared 4096×2048 BC7 sheets (8MB each, ~112MB all-resident); cost is per-SHEET not per-icon,
   and the rep system already uploads whole sheets, so per-item = different UV rect into an already-
   resident sheet (~0 extra VRAM, +16MB worst case). SRV budget fine (1 slot/sheet, 14«256). g_item_icon_
   layout already holds all 4844 iconId→rect. Caveat: ER GPU streaming not shareable → we keep our own
   sheet copy, but that's already paid by the rep system.
4. **Baked atlas removal** (`generated_shared/goblin_overlay_icons.{hpp,cpp}` ATLAS_PNG ~135KB + ICON_CELLS
   + the stbi_load_from_memory at goblin_overlay.cpp:1163 + tier-3 in IconSet::resolve). GATED on proving
   mod-agnostic native coverage first (the 7 POI + ERR-named map symbols are ERR-only; on vanilla they'd
   fall to circle — acceptable under the directive, but confirm intent). Keep stb_image (msbe_parser needs
   stbi_zlib_decode_buffer). Circle becomes the sole fallback.

## Key findings / non-obvious
- The 7 mod-added POI (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell Bearings /
  Interactables / Spiritspring Hawks) have NO ERR-custom glyph: massedit iconIds (374+) point to glyphs
  absent from all current menu files (numeric glyphs cap at 261). Recover via a real SB_MapCursor glyph
  (e.g. summon→89) where one fits, else circle.
- `MENU_MAP_ERR_*` (boss/grace) are ERR-only names; on non-ERR they don't resolve → circle if baked gone.
- Offline KRAK decompress on Linux WORKS via `internals/launcher/liboo2corelinux64.so.9` (extractor uses it).
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.

## Open decisions
- Bonus-2 summon glyph: `MENU_MAP_89` vs `MENU_MAP_21`.
- Is non-ERR/vanilla a hard support target? (decides whether baked can fully go or stays as non-ERR net.)
