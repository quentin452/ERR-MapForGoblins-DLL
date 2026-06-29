---
name: category-icons-00solo-atlas
description: "Category markers now draw the game's REAL item icon from the menutpfbnd:/00_Solo/ MENU_ItemIcon atlas (the \"other DCX\"), derived live per category. item_real_icon_id reads iconId from EquipParam@offset. Branch feat/category-icons-from-itemicon-atlas."
metadata: 
  node_type: memory
  type: project
---

The loot/item category markers can show the game's OWN inventory icon (not the hand-drawn baked
atlas). Source = the **`menu/hi/00_solo.tpfbhd`** item-icon atlas (`menutpfbnd:/00_Solo/`, the
`MENU_ItemIcon_<id>` sheets) — a DIFFERENT DCX than the map-point legend sheet (`01_common`) we
harvest for graces/bosses. <user>'s correction "trouve les icons à un autre DCX" + "on les load
manuellement déjà" pointed here. The loot categories have NO native map-point glyph (those are POI
symbols), so the item-icon atlas is the only real-art source for them.

SHIPPED on branch **feat/category-icons-from-itemicon-atlas** (commit 4893ae1, err builds clean,
**RUNTIME TEST PENDING**). Chain:
- `goblin::item_real_icon_id(key)` — reads the REAL inventory iconId LIVE from the owning EquipParam
  at the cross-verified offset: Goods@0x30, Weapon@0xBE, Protector@0xA6 (iconIdM; iconIdF@0xA8),
  Accessory@0x26, Gem@0x04. Provenance = `verify_equip_iconids()` + `self_calibrate_iconid()` (zero
  hardcoded, survives patches). NOTE: this is the read the deleted-ITEM_ICONS task originally asked
  for — useless for that dead path, exactly right for THIS feature. See [[item-icons-table-deleted]].
- Representative per category, derived at map build (map_entry_layer): most-common item key in each
  bucket → item_real_icon_id → `worldmap::category_rep_icon` (atomic store). NO hardcoded item ids —
  drift-proof to any mod. Logged `[CATICON]`.
- Load: `goblin::queue_force_item_icon(iconId)` → drained 1/tick in `res_tick_detour` via the SAME
  `run_create_icon` path graces use (so we drive residency, not wait). No-op until a CreateImage
  context is captured (g_ci_p1); gated by config nativeItemIcons. SAFE.
- Draw: `IconSet::resolve` → `overlay::native_item_icon(rep)` (re-added — harvested_icon +
  copy_sheet_cached), **falls back to baked atlas until resident → zero regression**.
- Panel: `category_is_gpu_native` folds in rep-icon; migrated list shows "item icon" + live residency.

⚠️ CENTRALIZED (commit c529707, RUNTIME TEST PENDING): the GPU-icon residency is now ONE system
after the per-category drain churned the repo + evicted the boss/grace map symbols (capped grace
force never brought them back → boss GPU died; <user> was furious). New design = a wanted-icon
registry (g_want_items / g_want_symbols) filled by gpu_want_item(iconId)/gpu_want_symbol("img://
MENU_MAP_ERR_Boss") + ONE engine-thread pump gpu_icon_tick (in res_tick_detour) that ALWAYS
re-forces the whole set round-robin (NO try-cap, self-heals eviction) via the HOOKED CreateImage
(force_symbol_hooked/force_item_hooked — fixes the old run_create_icon-via-ORIG 129->129 no-harvest
bug), then re-harvests twin-map (throttled). map build registers reps + world symbols (boss). Removed
the capped auto-force + per-category drain + queue_force_item_icon. Render resolve (native_item_icon /
native_map_point_icon_by_name) unchanged. RULE: never add a separate icon force path — register a
want, let the central pump handle it.

TWO GOTCHAS that hid the boss GPU icon for hours (commit 5ab6ec7, log-verified):
(1) MENU_MAP_* map-point pins (boss/grace) live in **repo+0x80** (harvest_repo_icons) — NOT the
repo+0xb0 MapTile twin (harvest_twin_map_icons). The pump must run harvest_repo_icons() or the forced
symbol is resident but never REGISTERED into g_map_icon_named. (2) the GFx resolve formats
"<symbol>_ptl", so the resident/registered name is **MENU_MAP_ERR_Boss_ptl** while the renderer looks
up the bare "MENU_MAP_ERR_Boss" (category_gpu_icon_name) → exact-match miss. Fix: store_map_icon_rect
registers the bare name too when the name ends in _ptl (+ map_icon_rect_by_name retries with _ptl).
Verified chain: [BOSSDIAG] named_keys_with_Boss=2 [MENU_MAP_ERR_Boss MENU_MAP_ERR_Boss_ptl]
bare_lookup_hit=true → [TEXMGR] cached 1024x1024 boss sheet → GPU. [BOSSDIAG] is a throttled
dumpIconTextures diag in gpu_icon_tick — remove once boss is confirmed stable.

✅ BOSS NOW VISIBLE deterministically (commit 2ea78b3) — after the _ptl/repo+0x80 fixes the chain
RESOLVED but drew INVISIBLE. Root cause = a SHARED COMMAND-LIST bug, not BC7/format/UV ([SYMDRAW]
proved fmt=98 BC7_UNORM, copied=true, valid rect/UV/handle). copy_sheet_cached used begin_icon_batch()
(no-op if the per-frame item-icon batch was already open → records the boss CopyResource onto an
already-open/closed shared g_command_list) + submit_and_wait() which NEVER clears g_icon_batch_open →
the copy frequently never executed → dest texture stayed undefined → transparent. NON-deterministic
(depended on whether item icons opened the batch that frame). Grace always worked because
ensure_grace_srv does its OWN unconditional g_command_list->Reset() (bypasses begin_icon_batch). FIX =
new ensure_map_sym_srv mirrors the grace path exactly (record_sprite_copy sub-rect + write_inline_srv,
dedicated reset, retry-until-ready); native_map_point_icon_by_name uses it. ✅ FIXED 2026-06-29 (build
fa25ab7, deployed): the latent copy_sheet_cached hazard CRASHED at map-open — the harvest path copies 2
ER atlas sheets back-to-back via begin_icon_batch()+submit_and_wait(); submit_and_wait Close()d the list
but left g_icon_batch_open=true, so sheet #2's begin_icon_batch() no-op'd (no Reset) and recorded
CopyTextureRegion/ResourceBarrier onto a CLOSED list → vkd3d-proton device-removed HARD crash, NO SEH
dump (log died right after two `[TEXMGR] cached ER sheet 4096x2048 ... (atlas)` lines, no
MapForGoblins_crash_*.txt produced). FIX = clear g_icon_batch_open at the END of submit_and_wait() itself
(one place, covers every one-shot caller); the manual clears in create_tex_from_dds_mem (line 571) and
flush_item_icon_batch (823) are now redundant but harmless. LESSON: invisible-but-resolves = suspect the
GPU COPY/command-list, not the texture; AND a missing-SEH-dump crash right after a TEXMGR copy = a
closed-command-list record, not a game-code fault.

ROADMAP (<user>, 2026-06-27, AFTER GAP#2 wire + entity-icon shipped on feat/dvdbnd-packed-reader —
commits 704147a data / b8c417e wire / 0e9016b entity-boss-icon; NOT pushed):
1. Keep hunting MISSING/unresolved category icons (the F1 A/B [baked]vs[runtime] panel + [CATICON] log
   are the oracle; a few categories still fall to baked — e.g. the 2 "wired, not resident" Keys need a
   2nd sheet load). Each category-rep that draws baked = a candidate to source natively.
2. DELETE the baked overlay atlas (generated_shared/goblin_overlay_icons ICON_CELLS) ONLY once EVERY
   category is proven to render without it (100% native coverage). It is currently the universal
   safety-net fallback (IconSet::resolve last tier) incl. the non-ERR case — do NOT remove early.
3. REFACTOR ERR-compat into its own file/folder for clarity. ERR-specific icon code = the MENU_MAP_ERR_*
   symbols touching THREE icon paths: Grace (GraceLayer s_grace_tex / MENU_MAP_ERR_GraceUnderground) +
   Boss (WorldBosses → MENU_MAP_ERR_Boss) + Entity (WorldHostileNPC → MENU_MAP_ERR_Boss @0.65x). The ONLY
   friction point = ERR ABSENT → those symbols don't resolve → boss/enemy fall back to the baked atlas
   (works, just not the ERR art). Isolate this ERR-vs-vanilla branch so the fallback story is one place.

DDS-FROM-DISK PLAN (<user> chose "runtime sblytbnd capture, no-bake" 2026-06-27): non-equipment item
icons never show because their 00_Solo sheets aren't streamed → CreateImage returns 0x0 (confirmed
[CREATEIMG] MENU_ItemIcon_475 -> 0x0). Equipment shows because its sheets are resident. Fix = draw the
icon from its sheet DDS ourselves. Existing C++ pieces (REUSE, don't rewrite): Oodle hook captures
decompressed DDS → g_dds_list; create_tex_from_dds_mem uploads a DDS blob → SRV. STAGED:
- ✅ GAP #1 DONE (commit 5515977, VERIFY [ITEMLAYOUT] log): item-icon LAYOUT captured no-bake from the
  sblytbnd — it's TextureAtlas **XML** (<SubTexture name="MENU_ItemIcon_<id>.png" x= y= width= height=/>
  under imagePath="<sheet>"), string-scanned in oodle_decompress_detour → g_item_icon_layout (iconId→
  sheet+rect). Public item_icon_layout_rect/count. gpu_icon_tick one-shot force_load_file("menu:/
  01_Common.sblytbnd") to trigger (loads at boot before hook). Same data as extract_subtextures.py.
  ✅ GAP #1 LAYOUT SOURCE SOLVED 2026-06-27 (RPM live RE, scripts <ghidra_scripts>\sblyt_hunt.py +
  sblyt_dump.py + tools/dump_item_icon_layout.py). TWO confirmed routes; disk is the complete one:
  - **RAM parse WORKS (overturns the old "freed=dead-end" verdict for resident atlases):** the
    decompressed TextureAtlas XML is resident as PLAINTEXT for the currently-loaded atlases —
    `<SubTexture name="MENU_ItemIcon_<id>.png" x= y= width= height=/>` under `<TextureAtlas
    imagePath="SB_Icon_*.png">`. sblyt_hunt found `<SubTexture` 200+/`imagePath` 7×/`MENU_ItemIcon`
    ASCII 200+ in the live heap; sblyt_dump slurped the one resident XML region → parsed 600 rects
    direct (NO ResCap-struct/+48 walk needed — scan the heap for the XML text). BUT only the 7
    ERR-custom atlases were resident (imagePath=7); the full base 47-layout buffer WAS freed after
    parse (confirms the old RE) → pure-RAM = PARTIAL + state-dependent.
  - **★ DISK decompress = the COMPLETE, mod-aware source (the "fallback", but it's the right answer):**
    oo2core-decompress the ACTIVE mod's `menu/hi/01_common.sblytbnd.dcx` → parse every `SB_Icon_*.layout`
    → iconId→(sheet,x,y,w,h). Vanilla disk = 3071 entries (id 0..41603); **ERR mod's own file
    `<windows_downloads>\ERRv2.2.9.6-541-...\ERRv2.2.9.6\mod\menu\hi\01_common.sblytbnd.dcx` = 4845 entries
    (id 0..46799) = base + ERR additions** (SB_Icon_ERR_00..03 + SB_Icon_ERR_Gem_00/01 incl. the 46xxx
    gems). RAM vs disk cross-validate EXACTLY (46799 → SB_Icon_ERR_Gem_01 3936,1804,160,160 identical).
  - ⚠️ CORRECTIONS: sheet is NOT iconId/1000 (`MENU_ItemIcon_%02d000`) — it's arbitrary per the layout
    (SB_Icon_00..08_dlc, SB_Chara_01 for 41xxx 270×270 great-rune/chara, a few SB_Log/SB_MainMenu_02);
    the layout XML is REQUIRED to map iconId→sheet. Item-icon DDS pixels (GAP #2) live in the 2.1 GB
    `00_Solo.tpfbhd/.tpfbdt` BHD5 archive (SB_Icon_*.png entries), NOT 01_common.tpf.
  - **RUNTIME WIRING (recommended, no-bake, mod-aware):** decompress the active mod's
    01_common.sblytbnd.dcx from disk via the held g_oodle_orig (resolve path off the live ResCap +18
    wstring / virtual-FS alias root, see [[virtualfs-alias-modroot-anchor]]) → existing XML parser
    g_item_icon_layout. Old force_load Oodle-recapture was dead (returns cached resource, no
    re-decompress); skip the ResCap +48 hash-table walk — unnecessary now.
  - ✅ ROUTE A SHIPPED + RUNTIME-VALIDATED 2026-06-27 (branch feat/category-icons-from-itemicon-atlas,
    build clean; log `[ITEMLAYOUT] from disk sblytbnd: 4844 MENU_ItemIcon rects` — matches the offline
    4845 minus iconId 0, excluded by the parser's id>0 guard). Generic primitive
    `goblin::worldmap::read_game_file_decompressed(rel)` in
    loot_disk.cpp/.hpp: resolve_root_file ancestor-walk (mod overlay first, then UXM game) → slurp →
    msbe::dcx_decompress(+resolve_oodle) → raw bytes (BND4/TPF/…). REUSE for GAP#2 DDS later (just
    point it at 00_Solo.tpfbhd). `goblin::load_item_icon_layout_from_disk()` (goblin_inject.cpp) reads
    "menu/hi/01_common.sblytbnd.dcx" → parse_item_icon_layout → g_item_icon_layout. Wired in
    gpu_icon_tick (one-shot detached std::thread, ≤5 spaced retries, replaces the dead force_load).
    Verify logs: `[GAMEFILE] …01_common.sblytbnd.dcx -> N bytes (KRAK)` + `[ITEMLAYOUT] from disk
    sblytbnd: ~4845 MENU_ItemIcon rects`. ⚠️ This completes the RECT TABLE only — drawing a
    non-resident icon still needs GAP #2 (the DDS sheet pixels from 00_Solo.tpfbhd) + the WIRE step.
  - ✅ PACKED-fallback RUNTIME-VALIDATED 2026-06-27: env-var test affordance `MFG_TEST_NO_GAMEFILE=1`
    in read_game_file_decompressed (mirrors map-dir __test_fallback__) forces "not found" WITHOUT
    touching game files → simulates packed-vanilla on an unpacked box. Run via the dropped `TEST -
    Simulate Packed (no gamefile).BAT` (ERR root): log showed `[GAMEFILE] …simulating packed (not
    found)` + `[ITEMLAYOUT] disk sblytbnd unavailable (0 bytes)`, fired 5× then STOPPED (the
    s_layout_tries<5 bound), overlay kept refreshing markers, NO crash → graceful atlas fallback
    confirmed. Real-packed (never-UXM install) still untested but the code path is proven. CONCLUSION:
    the disk no-bake item-layout shares the EXISTING disk-loot loose-file dependency — works for mod
    overlays (the real usage) + UXM; only pure-packed-no-mod is empty (== disk-loot's disk=0 case).
  - ⏳ PACKED FIX = EARLY OODLE-HOOK (<user> chose "cheap, test, escalate"; SHIPPED 2026-06-27 build
    20:53, RUNTIME-TEST PENDING). The oodle_decompress_detour ALREADY parses the sblytbnd's BND4 when
    it fires; the fix = arm the IAT hook EARLY enough to catch the game's ONE-TIME boot decompress of
    the menu sblytbnd (works on packed/encrypted installs — the game decrypts before Oodle, we just
    observe). Changes: goblin::install_oodle_hook() exposed + regated on nativeItemIcons (was
    dumpIconTextures-only); CALLED IN DllMain right after load_config (before menu boot) — the deferred
    init_icon_tex_probe re-call is now a no-op; g_item_layout_captured atomic stops the per-BND4 needle
    scan once captured (bounds cost to boot). DECISIVE TEST = run the MFG_TEST_NO_GAMEFILE=1 bat (disk
    reader off, Oodle hook unaffected): if `[ITEMLAYOUT] parsed N rects` appears DESPITE `[GAMEFILE]
    simulating packed` → Oodle catches boot → packed solved cheap; if only the packed-unavailable
    warnings → boot timing missed → ESCALATE to the virtual-FS raw-file-read RE (DLFileDevice/CSFile
    read-to-buffer → dcx_decompress). ⚠️ residual risk if Oodle misses boot in REAL packed (no env
    var): disk reader fails without setting captured → detour scans every BND4 forever; add a
    time/decompress-count cap THEN (deferred until the test says the hook even catches boot).
- ✅ GAP #2 DATA SOLVED + C++-VALIDATED 2026-06-27 (branch feat/dvdbnd-packed-reader). The sheet DDS are
  NOT in 00_Solo.tpfbhd (BHF4, no SB_Icon entries) — they're in **menu/hi/01_common.tpf.dcx** (the
  monolithic "brick" route): AES-encrypted-in-dvdbnd + DCX-KRAK → **194 MB PC TPF**, 56 textures, 13
  SB_Icon_* sheets = full DDS files, **BC7 / DX10 header, 4096x2048 (~8 MB each)**, TPF flags1=0 (no
  per-entry DCX). New C++ (offline-validated byte-exact, SHA256, vs a Python probe on the real packed
  install): `msbe::tpf_find_texture(buf,n,name,&off,&len)` (PC TPF, UTF-16 names, 20-byte entry) +
  `worldmap::read_item_icon_sheets(names)→map<name,DDS>` (reads 01_common.tpf.dcx via
  read_game_file_decompressed = loose→packed dvdbnd, decompresses ONCE, copies out each sheet, frees the
  194 MB). The packed reader's untested AES-128-ECB path is now PROVEN here (00_solo.tpfbhd→BHF4,
  01_common.tpf.dcx→valid DCX). ⚠️ MAPPING: the layout's `sheet` = the imagePath verbatim **WITH .png**
  (e.g. "SB_Icon_00.png"); the TPF texture name has NO extension ("SB_Icon_00") → the WIRE must strip the
  trailing ".png". All 47 imagePaths ↔ 47 TPF SB_* entries 1:1. See [[dvdbnd-packed-reader]].
- ✅ GAP #2 WIRE SHIPPED 2026-06-27 (branch feat/dvdbnd-packed-reader, UNCOMMITTED, build+deploy clean,
  IN-GAME TEST PENDING). native_item_icon (goblin_overlay.cpp): (1) resident harvested sheet first
  (unchanged); (2) NEW fallback on miss → item_icon_layout_rect(iconId)→sheet+rect, strip ".png" →
  ensure_disk_sheet(name) → tex+UV (rect/sheetDims). Disk-sheet machinery (anon ns): g_disk_sheets
  (name→{gpu,w,h,state}) + g_disk_sheet_dds (bg-loaded DDS) + request_disk_sheets() (one-shot detached
  thread runs read_item_icon_sheets OFF the render thread — 194 MB decompress) + ensure_disk_sheet()
  (render thread: upload a pending DDS via create_tex_from_dds_mem, cache SRV, free CPU DDS; else
  register want + kick loader, baked fallback meanwhile). FIXED the shared-command-list hazard:
  create_tex_from_dds_mem now does a DEDICATED allocator/list Reset (not begin_icon_batch) + clears
  g_icon_batch_open after submit (keeps the invariant). DX10/BC7 path confirmed (dxgiFormat 98,
  bc_block_bytes 16, size == mip0-only). Renderer already calls native_item_icon(category_rep_icon)
  with baked fallback (map_renderer IconSet::resolve). RUNTIME WATCH: [ITEMSHEET] extracted N/M +
  [TEXMGR] uploaded DDS 4096x2048 fmt=98; non-equipment category icons now draw real art. RISKS to
  check in-game: UV alignment (layout rect res vs DDS res), SRV slot budget (<256), 1-frame hitch on
  first sheet upload.
- ✅ GAP #2 RUNTIME-VALIDATED IN-GAME 2026-06-30 (commit e4fc128) — map-only, NO inventory: log
  `[ITEMLAYOUT] from disk sblytbnd: 4844 rects` → `[ITEMSHEET] extracted 2/2, 4/5` → `[TEXMGR] uploaded
  DDS 4096x2048 fmt=98` ×6, ~47 item icons recovered, NO crash. The crash that blocked the first attempt
  was the copy_sheet_cached closed-command-list bug (now fixed, commit 0df36a3 — see the ✅ FIXED note
  above). ⚠️ BOOTSTRAP-PATH TRAP (was the "no icons resolved" symptom): load_item_icon_layout_from_disk()
  lived inside gpu_icon_tick, which only runs once res_tick_detour (FUN_140d724c0) captures the
  menu-resource manager — an INVENTORY/menu event. A map-only session never fires it (CreateImage hook +
  g_ci_p1 capture is a DIFFERENT hook and fires from the map, so [CREATEIMG] is NOT proof the layout path
  ran), so the rect table stayed empty (zero [ITEMLAYOUT]) and every rep fell to the baked atlas. FIX =
  the disk load is pure file IO (no CreateImage/manager dep) → hoisted to background_harvest_tick()
  (worldmap-probe thread, runs from init every 100ms regardless of menu), gated nativeItemIcons +
  count==0/tries<5/!inflight detached thread. RULE: any menu-independent bootstrap (disk reads, layout
  parse) belongs on the worldmap-probe thread, NOT behind the res_tick/g_ci_p1 gates. ⏳ minor follow-up:
  one `[ITEMSHEET] '' not in 01_common.tpf` (a rep with empty/missing imagePath → ".png"-strip yields "")
  → 4/5 that pass; harmless, the rep just stays baked. Track down the empty-sheet layout entry.
- ⚠️ DEV-HOOK GATING (still true): force_load_file + install_oodle_hook are dumpIconTextures-gated; the
  Oodle-detour layout-capture (oodle_decompress_detour) is therefore OFF in production, so the disk
  load above is the SOLE production layout source. Fine now that it is hoisted + menu-independent.

OPEN QUESTION for the runtime test: does the manual force-load (run_create_icon on MENU_ItemIcon
without an explicit 00_Solo group-load) actually make item-icon SHEETS resident → harvestable? The
RE doc windows_resident_icon_enumeration_re_findings.md calls atlas-streaming fragile and a past note
said "force-load=atlas-no-rects". The [CATICON] log + the panel's per-category residency tell us the
real hit-rate. If low → next step = add the 00_Solo group-load (g_bind_action) before the CreateImage.
