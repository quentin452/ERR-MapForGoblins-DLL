# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.
Last updated: 2026-06-30 (per-item icons + bench spike + map-exit bug triage session).

## Session recap (2026-06-30 PM) — shipped + open
- DONE `07b3904` bonus-2: SummoningPools → MENU_MAP_89 (Martyr Effigy), verified in-game (246 pools, live param).
- DONE `caed7ef` per-item loot icons: lot → real EquipParam iconId, native_item_icon → rep → atlas → circle. Verified working.
- DONE `8c16b60` bench lag-spike WARN (relative-to-baseline, even for quiet timers) + spikes column.
- DONE `d792a3a` instrument `draw_minimap_hud` as `render.minimap`. RESULT (run 13:10): spikes only
  ~3ms (`~600x` a 0.01ms avg) — **minimap EXONERATED**, not the felt map-close lag.

## OPEN — deferred for later (2026-06-30)
1. **Lag-spike hunt — real suspect `refresh.collected.*`.** The minimap was a red herring. The collected-
   state refresh spikes in the SPIKE log (earlier run: `refresh.collected.read_wgm` 2–5ms, ~30x its avg).
   It is SUPPOSED to already use a good lookup, but the spikes say otherwise — re-audit the collected
   lookup (read_wgm path) for a per-marker / per-frame O(n) hidden cost or a cache miss. Use the new
   `[SPIKE]` warns + the spikes column to localize. Not yet root-caused.
   - Cosmetic: spike ratio prints `~600x its 0.01 ms avg` when the baseline rounds to ~0 — divide-by-tiny.
     Harmless (still a real spike), tidy later (floor the avg in the ratio display).
2. **Map-exit input softlock + F1 mouse-dead.** Root NOT WndProc (keyup passes). Prime suspect: DI hooks
   blank buffered key-UP while F1 open → game misses release → stuck movement "à vie". Plus no map-close
   cleanup edge for ImGui (mouse-dead half). Needs Windows runtime RE before patching →
   `docs/re/windows_input_softlock_re_prompt.md`.

## Active branch
`feat/native-poi-icons` (off `feat/dvdbnd-packed-reader` off `master`). NOT pushed. Build+deploy on Linux
run **sandbox-disabled**; deploy target `<ERR_ROOT>/dll/offline/MapForGoblins.dll` ONLY (ERR_ROOT=
`/home/iamacat/Games/ERRv2.2.9.6`; do NOT also touch `MapForGoblins_vanilla.dll` — see double-load bug).
Verify deployed md5 == build output after every deploy. Last build deployed: `352b586` (bonus-1).
**Uncommitted on the branch:** bonus-1 src (5 files: goblin_inject.cpp/.hpp, goblin_overlay.cpp/.hpp,
worldmap/map_renderer.cpp) — built + deployed, NOT committed (was pending in-game test; bonus-1 is now
exonerated of the grace bug, so it is commit-ready once a single-DLL run confirms it draws).

## feat/native-poi-icons — FOLLOWUP to finish this branch
1. ✅ DONE — bonus-1 retested single-DLL + committed (`6e45986`): undiscovered grace draws
   `MENU_MAP_Player_02` from disk (`[GRACEUNDISC]` tex=0x33000001c0, UV correct), discovered = bonfire+check
   (GRACE-SPRITE LOCKED ×32, fine on single-DLL). The disk map-point render path (`map_point_glyph_uv` +
   accessors) is now in — bonus-2/3 reuse it.
2. NEXT — bonus-2 (summon glyph), then per-item icons, then baked removal (see QUEUE below).
3. Push when the user says so (nothing pushed yet this whole arc).

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

## DONE but uncommitted
- **Bonus-1: undiscovered-grace icon.** Disk map-point render path built: `goblin::map_point_rect_by_name`/
  `map_point_rect` accessors (goblin_inject) + `goblin::overlay::map_point_glyph_uv` (goblin_overlay,
  mirrors native_item_icon's GAP#2 disk branch) + map_renderer grace block draws `MENU_MAP_Player_02`
  (gold figurine, 286,956,42,62 SB_MapCursor) from disk when undiscovered, bonfire+check when discovered.
  Built (`352b586`), deployed, NOT committed. The undiscovered figurine renders; the "discovered grace
  disappears" report was a DOUBLE-LOAD artifact (now understood). Needs a clean single-DLL retest, then commit.

## QUEUE (ordered)
1. ~~**Bonus-2: SummoningPools → native glyph.**~~ DONE (uncommitted). `MENU_MAP_89` (Martyr Effigy)
   chosen. category_meta.cpp: `WorldSummoningPools → 89` in `CATEGORY_GPU_ICONS`. Plus a generic disk
   fallback in `MapPointProvider::resolve` (map_renderer.cpp) — resident GPU symbol, else `map_point_glyph_uv`
   by iconId, mod-agnostic. Verified in-game (246 pools, live param, Martyr Effigy shows). Committed
   `07b3904`. NOT pushed.
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

## Known bugs (priority)
- **ROOT CAUSE — DOUBLE-LOAD of two DLL variants.** The install ships `MapForGoblins.dll` (ERR build) AND
  `MapForGoblins_vanilla.dll` (vanilla build). When both are present BOTH load → two in-process instances →
  doubled everything: double ImGui draw, double MsgRepository PlaceName patch (log shows 36096→36624; a
  single instance patches once to 36096), double hook installs (the `[ICONTEX] CreateImage hooked` followed
  by `AOB not found` was the 2nd instance hitting an already-patched prologue — NOT MinHook re-patch),
  discovered-grace markers vanishing, and contradictory logs. CONFIRMED by log diff: single-DLL run =
  `AOB not found`=0, one PlaceName patch (36096), no `?PlaceName?`; the user observed all the "weird bugs"
  fixed with one DLL. `?PlaceName?` / double-draw / grace-discovered-missing were ALL double-load artifacts,
  NOT real code bugs (bonus-1 is exonerated).
  - **FIX (immediate):** only ONE DLL in the load path — stop shipping/deploying `MapForGoblins_vanilla.dll`;
    the launcher (`ReforgedLauncher`, via `internals/modengine/*.me3`) should load `MapForGoblins.dll` only.
    (Renaming just `MapForGoblins.dll` does NOT disable the mod — the launcher falls back to the stale
    `_vanilla.dll`. To truly test mod-off, remove BOTH.)
  - **FIX (strategic) = SOLE DLL.** The per-mod build split (ERR vs vanilla) exists only because the code has
    ERR-frozen assumptions (baked atlas, ERR-named symbols) that break on vanilla. Making the DLL truly
    mod-agnostic (the prime directive: read the active install's files at runtime) means ONE build serves ERR
    + vanilla + any mod → `_vanilla.dll` becomes unnecessary → no variant → no double-load. "Sole DLL" is a
    direct consequence/goal of the prime directive; "DLL per mod" IS the anti-pattern.
  - **FIX (hardening, TODO) = runtime HARD double-load check.** Turn silent double-load chaos into a clear
    user-facing error instead of double-draw/`?PlaceName?`. At init (DllMain/early), acquire a named mutex
    (e.g. `CreateMutexW(Local\\MapForGoblins.instance)`); if `GetLastError()==ERROR_ALREADY_EXISTS`, this is
    a SECOND instance → **early-return from init: install NO hooks, NO ImGui, NO MsgRepository/PlaceName
    patch** (so nothing is doubled), and set a shared "double-load detected" flag. The single ACTIVE
    instance, on F1/overlay open, renders a prominent error banner instead of the map:
    "⚠ Double load detected — two MapForGoblins instances loaded (likely `MapForGoblins.dll` +
    `MapForGoblins_vanilla.dll`). Check your me3/launcher config and remove the duplicate." Player DX: one
    clean ImGui instance (the surviving one) + an actionable message, never a corrupted double overlay.
    (Alternative "keep-last, disable-earlier" is more fragile than "first-wins, second-bails" — prefer the
    mutex bail.) This is defensive insurance; the real fix is sole-dll, but the guard protects users with
    a misconfigured install.
- **`?PlaceName?` everywhere = a DOUBLE-LOAD artifact (root cause above), NOT an FMG-scale bug.** Evidence:
  a SINGLE-DLL run patches PlaceName ONCE (36096 entries) and shows NO `?PlaceName?`; a DOUBLE-load run
  patches TWICE (36096→36624) and shows `?PlaceName?`. The 2nd instance re-patches the slot-19 FMG that the
  1st instance ALREADY swapped to `g_expanded_placename_fmg` — it treats the already-expanded FMG as its
  "vanilla base" and re-expands it, misaligning the id-range groups → base place-names corrupt → `?PlaceName?`.
  Confirmed NOT a disk/Steam file: an inotify monitor of the Steam ELDEN RING dir during "verify integrity"
  showed ZERO game-file rewrites (only Steam's own `.temp_write_*` probes); "Steam verify fixes it" = the
  game RESTART. **PRIMARY FIX = single DLL (the sole-dll cleanup above) — already resolves it.**
  - **Discovered-grace-missing was the same double-load artifact, NOT bonus-1.**
  - **Optional hardening (LOW priority, directive-aligned) = lookup-not-patch:** make `lookup_text(id)`
    (goblin_messages.cpp:1054) DECODE the offset id (500M→GoodsName slot10, 100M→Weapon11, 200M→Protector12,
    … decode infra already at goblin_inject.cpp 1212/5082) and read the SOURCE MsgRepository slot DIRECTLY,
    then DELETE the expanded-FMG build + the slot-19 swap (`*g_placename_slot_ptr=…`, goblin_messages.cpp:1044).
    Makes the double-patch structurally impossible AND removes all PlaceName mutation (pure runtime lookup,
    directive-compliant). Overlay labels (self-rendered AddText) keep working. Not urgent now that single-DLL fixes it.

## Parallel workstreams (plan-only branches — NOT this branch)
These branches exist with a plan attached but little/no implementation; pick up via their plan docs.
- `feature/dx-bugs-backlog` → `docs/memory/process/plan-dx-bugs-audit.md`, `docs/memory/bugs/dx-bugs-backlog.md`
- `feature/spatial-grid-opti` → `docs/memory/process/plan-spatial-grid-audit.md`
- `feat/quests` (quest NPC layer; bonus-3 `MENU_MAP_80` lands here) → `docs/memory/process/plan-quests-audit.md`,
  `docs/feat_quests_implementation_plan.md`, `docs/memory/features/quest-browser.md`
- Plan registry: `docs/memory/process/plans-to-audit.md`.

## Debake candidates (apply the prime directive — replace ERR-frozen bakes with active-file reads)
- **Item-name localization (FR/EN) → INVESTIGATED, KEEP THE BAKE.** Item names are ALREADY read from the
  active-language FMG at runtime (`lookup_text_utf8(m.loc)`, goblin_overlay.cpp:2233); only the cross-language
  ENGLISH alias is baked (`src/generated/goblin_name_aliases_en.cpp`, ~3276 entries, from
  `tools/generate_data.py`). Full debake not worth it: the game loads ONLY the active language's msgbnd, so
  showing FR+EN simultaneously (when game=EN) would need reading a non-resident msgbnd off disk (~10MB Oodle
  decompress, latency). The bake is small + battle-tested. LOW-effort partial option (read active-lang from
  FMG, drop the baked alias for the active language only) = minor win. Verdict: leave as-is.
- **Overlay icon atlas** (the big one) — see QUEUE item 4.

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
- ~~Bonus-2 summon glyph: `MENU_MAP_89` vs `MENU_MAP_21`.~~ RESOLVED → `MENU_MAP_89` (Martyr Effigy).
- Is non-ERR/vanilla a hard support target? (decides whether baked can fully go or stays as non-ERR net.)
