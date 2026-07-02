# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping note (2026-07-01, done):** this file had grown to 1264 lines, mostly stale entries
for work long since merged + in-game confirmed, whose content already lived in `docs/changelog.md`
or a `docs/plans/*.md` file marked COMPLETE. Backfilled 2 real gaps found in `changelog.md` (the F3
Alt+Tab keyboard-dead fix, the minimap search-hit edge-clamp + search-hint fix — both now under
`[Unreleased] > Fixed`), then deleted every HANDOFF section whose user-facing content was confirmed
present in changelog, or whose pure-refactor content was confirmed present in a COMPLETE plan doc.
Kept: genuinely live/in-progress work, open questions, and standing knowledge (gotchas, deferred
decisions, non-obvious facts) not fully captured anywhere else. If you're looking for the history of
something not below, check `docs/changelog.md` first, then the relevant `docs/plans/*.md`.

Last updated: 2026-07-02 (hot-reload D + Phase 3 RPC + Phase 4 loop ALL merged & in-game validated
on Linux/Proton; spiderfy v1+v2, in-game pause, RPC input injection/HUD, NPC altitude badges, F2
repro — see the Phase 4 sections below. Earlier same day: SINGLE-DLL migration + the 9 native-pin
parity landmark categories in-game verified.)

## RESUME HERE (2026-07-02) — hot-reload Slice D IMPLEMENTED (`feat/overlay-hotreload-slice-d`), Windows in-game validation next

Slice C was already merged to `master` (`af6baf7`, part of the `ed0a0e9` merge — the old "not yet
merged" note here was stale). Slice D (file-watcher + actual FreeLibrary/LoadLibrary reload cycle)
is now IMPLEMENTED on `feat/overlay-hotreload-slice-d` and build-verified on Linux (both configs
link clean; all exports verified). Full design + audit detail in
`docs/plans/overlay_hot_reload_playwright_plan.md` (Slice D section). Highlights:

- **Reload cycle:** host-side 500ms watcher thread polls the built `goblin_overlay_render.dll`
  (stable mtime+size + exclusive-open = linker done) → `maybe_reload()` at the top of `hk_present`
  swaps between frames: copy to `goblin_overlay_render.hot<gen>.dll` (Windows locks loaded module
  files — the FIRST load is a copy too), LoadLibrary + re-GetProcAddress, old module's FreeLibrary
  deferred one reload (worker-tail grace), new module's `prebuild_markers` + census re-run (fresh
  statics). SRW lock: shared in every loader `call_*`, exclusive during swap; detached disk-build
  worker gated via new `MFG_RenderIdle` export; `loot_disk`'s `g_build_trigger` (the one host-held
  render fn ptr) nulled before / re-registered after.
- **Biggest audit finding — LATENT SLICE C BUG fixed wholesale:** /MT = per-DLL CRT heaps, and ~8
  `overlay_api` functions pass std::string/vector across the boundary (by value / move / out-param)
  = alloc one heap, free the other. Fix: `src/goblin_render_new_override.cpp` (render-target-only)
  forwards the render DLL's global operator new/delete to host exports `MFG_HostAlloc/Free` — one
  heap for all C++ allocations, whole surface safe by construction. ImGui allocates via malloc (not
  new) → host allocator triple travels in `OverlayFrameCtx`, trampolines `SetAllocatorFunctions`
  once per module load.

**ALL IN-GAME VALIDATED 2026-07-02, ON THIS LINUX BOX (ERR under Proton)** — the "split-build
validation is Windows-only" assumption was WRONG (LoadLibrary/FreeLibrary + TCP RPC work under
Wine). Split build booted clean (SIG 29/29, disk build crash-free = the Slice C cross-DLL
loot_disk path's first live run), 3 live reloads (RPC gen1, watcher-auto gen2+gen3, swap 1.3s
after copy), and the full dev loop closed: edit render source → rebuild render target →
auto-swap → screenshot shows the change in the RUNNING game. Gotcha hit: first launch crashed
from the STALE pre-`/Z7` `build-linux-hotreload` cache — fresh reconfigure
(`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under CMake 4) fixed it; watch for any recurrence (PDBs now
deploy alongside for symbolized triage). Windows spot-check now optional.

**Phase 3 debug RPC — IMPLEMENTED + IN-GAME VALIDATED same day (merged with the above).**
TCP loopback (NOT named pipe — Wine loopback == host loopback, so a Linux Python driver reaches the
Proton game directly), gated on ini `[Debug] debug_rpc_port` (empty = off). Commands ALL validated
live: `ping`, `status` (incl. render generation — poll to watch a reload land), `open_f1`,
`set <ini_key> <value>` (generic, through the ini schema via new `goblin::config_set_by_key`),
`screenshot <path>` (backbuffer→BMP, includes the overlay — captured the F1 panel it had itself
opened), `reload_overlay`. Listener thread queues; commands execute on the present thread
(`debug_rpc::pump` at the end of `hk_present`). `search` deferred (render-side static buffer,
needs a new export). Driver `tools/mfg_rpc.py` (`wait-reload` polls the gen bump). NB screenshot
paths are the GAME's view: `Z:\tmp\...` under Proton. **The deployed ERR ini
(`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.ini`) keeps `debug_rpc_port = 38700` set** for
future sessions; deploy dir restored to the single-DLL daily driver (render DLLs removed).
**Phase 4 — FIRST REAL LOOP RUN COMPLETE (2026-07-02, `feat/phase4-device-aware-hints`):** shipped
backlog item 2's open UI half (device-aware close-hint — F1 panel shows the configured gamepad
combo, e.g. "Y+R3 close", when the pad is the active device) with BOTH branches visually verified
in the running game via hot-reload + RPC screenshots (gamepad branch tested by hot-reloading a
condition-forced build, then reverting — 3 live iterations, ~15s each, no game restart).
**Second split-boot crash root-caused + fixed on the way** (the Phase 3 "stale cache" theory was
wrong): per-DLL spdlog registries — render-side logs lazily created render's own default logger
inside hooked paths (intermittent boot AV, PDB-symbolized in `should_log`) AND vanished from
MapForGoblins.log entirely. Fixed with a render→host log bridge (`goblin_render_log_bridge.cpp`
`MFG_RenderInitLogging` → host `MFG_HostLogLine`), initialized by the loader as the first render
call; validated crash-free with 300 render lines landing in the host log. Loop constraint noted:
without a loaded save only title-screen/F1-panel targets are verifiable — an RPC game-input /
save-load command is the next unblocker for worldmap targets (F2 pan-clamp, hover spiderfy).
Remaining nit: keyboard branch hard-says "F1" even if `overlay_toggle_key` is rebound (needs a
reverse vk→name helper).

**RPC input injection + in-game pause — IMPLEMENTED + IN-GAME VALIDATED (2026-07-02,
`feat/rpc-input-injection`):** the loop's "can't load a save" constraint is CLOSED. RPC gained
`key <name> [hold_ms]` / `mouse_move` / `mouse_click` (SendInput on the listener thread; **AZERTY
gotcha: character keys must be sent VK-only, KEYEVENTF_SCANCODE makes letters vanish under Wine +
non-QWERTY layout**) — full nav validated: title → Continue → in-world → Equipment via `key E` →
**worldmap via `key M`** (this install's bind; vanilla-default G is not it) → `status map_open=1`.
Plus backlog item 4 shipped: `goblin_pause` (elden_pause je-flip, sig `PAUSE_BRANCH`, 30/30 SIG
clean), F1 checkbox + RPC `pause 0|1|toggle` + `paused=` in status — validated by screenshot diff
(paused = 0 changed pixels over 3s vs ~4.8k running). The user's "stuck in pause when the game is
backgrounded" mystery: most likely PauseTheGame.dll (reads its keybinds via GLOBAL
GetAsyncKeyState → typing its pause key in ANOTHER app toggles pause) — now redundant with our
pause; **recommend removing PauseTheGame.dll from the me3 profiles**. A driver clears any pause
via `pause 0` before scripting. Next unlocked: worldmap-target Phase 4 loops (spiderfy, F2).

## Overlay i18n v1 — SHIPPED + in-game validated FR (2026-07-02, `feat/overlay-i18n-v1`)

`goblin::i18n::tr()` (host module, GOBLIN_RENDER_API-exported) + ini `overlay_language`
(auto|en|fr|…) + disk table `lang/<code>.txt` (en=/tr= pairs, \n-escaped; missing → EN
fallback). ~180 tr() sites across the panel section files + map_renderer tooltip glue;
category/section labels translated at display (incl. the alphabetical sort + the category
search box matching EN or FR); `Filter::match` also matches translated per-block keyword
strings so the settings search works in both languages ("echelle" → Marker scale, validated
in-game). `assets/lang/fr.txt` ships ~270 translated strings; build.bat packaging copies
`assets/lang/*.txt` into the package (ERR: `dll/offline/lang/`; profiles: `MapForGoblins/lang/`)
— **packaging path NOT yet exercised on the Windows box** (next `build.bat snapshot` run
verifies it). Gotchas: (1) `auto` reads the WINE prefix locale under Proton (usually en_US
even on a French desktop) → French users set `overlay_language = fr` explicitly — the
deployed dev ini has it; (2) avoid `œ` in translations (outside the merged font ranges —
use "oe"); (3) translated format strings must keep the % placeholders in order. Deliberately
NOT translated (v2 candidates): quest-browser step CONTENT (hand-authored corpus, big),
dev-only sections (P2b/sprites/grace-debug/dev-tools), spdlog lines. The same lang-file
mechanism can carry future languages — drop a `lang/de.txt` etc., no rebuild.
**Live switch added same day (`feat/i18n-live-switch`, in-game validated fr→en→fr):** F1
"Language:" combo (under the master row; entries = auto, en, + one per `lang/*.txt` scanned
while the combo is open) calls `i18n::set_language()` — same-frame table swap, no restart.
Safe WITHOUT locking because every tr()/set_language caller is on the PRESENT thread (panel,
renderer, RPC pump — documented in goblin_i18n.hpp; keep it that way). `generation()` bumps
per load; the category sort order re-sorts on it (order follows the displayed labels). RPC
`set overlay_language <code>` also swaps live (wired in config_set_by_key — how the
validation drove it). Persist = the normal "Save to INI".

## Clang-only Phase 1 — WINDOWS BUILD + SNAPSHOT VALIDATED (2026-07-02) → only the in-game matrix left

`build.bat` is now ninja+clang-cl (no VS/msbuild; tool paths env-overridable, defaults = the
Windows box per `build-toolchain-clang-xwin.md`); `/Brepro` determinism PROVEN on Linux (relink →
identical md5); PDB pairs archived to `pdb-archive/<ver>-<profile>/`; `tools/lint_seh.py` guards
the SEH-elision regression. Old `build/` msbuild dir is disposable (`build-err/` replaces it).

**Default build VALIDATED on Windows (2026-07-02):** `build.bat` (ERR) ran clean end to end —
auto-configure (CMake 4.1 + Ninja + `clang-cl-xwin.cmake`, Clang 22.1.8), `[80/80]` compile+link →
`[SUCCESS]`, `build-err/{MapForGoblins.dll 4.6 MB,.lib,.pdb}`. Points to note:
- **0 real errors.** The only `Failed` line is `Performing Test CMAKE_HAVE_LIBC_PTHREAD - Failed` —
  expected on Windows (no pthread), CMake falls back correctly.
- **340 warnings, all benign / third-party**, two dominant recurring sources: (1)
  `-Wdeprecated-literal-operator` on `operator"" _a` inside vendored spdlog bundled-fmt; (2)
  `-Wdeprecated-declarations` on `std::wstring_convert`/`<codecvt>` in `src/from/params.hpp:17`
  (deprecated C++17, still functional). Both suppressible if we ever want a clean log
  (`_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING`); not worth churn now. (`snapshot` adds imgui
  `-Wnontrivial-memcall` on its `memset(this,…)` ctors — same benign class.)

**`build.bat snapshot` VALIDATED on Windows (2026-07-02) — packaging + PDB archive proven.** Full
`pre-1.0.18` (ERR) snapshot ran end to end: data pipeline (964 MSB, 28313 placements, 0 MSB errors)
→ reconfigure `-DVERSION_PRE=pre` → `[64/64]` link `[SUCCESS]` → `mfg_inigen` INI → package under
`pre-release/` (`dll/offline/{dll,ini}` + `addons/MapForGoblins/menu/02_120_worldmap.gfx` + LICENSE)
→ **PDB pair archived to `pdb-archive/pre-1.0.18-err/` (DLL + 27 MB PDB, NOT shipped in the package)**
→ README version-substituted (`vpre-1.0.18`). Crash-symbolication chain verified: shipped DLL is
**byte-identical (SHA-256)** across `pre-release/`, `pdb-archive/`, `build-err/`.

**Still open:** `build.bat release` (version-bump path) un-exercised; then Phase 2's real in-game
validation matrix. Docs already flipped to "clang = canonical" + `steam_api64.lib` removed. See
`docs/plans/clang_only_toolchain_plan.md` Phase 1/2.

## Two new plans scoped (2026-07-01): big-files refactor + clang-only toolchain

`docs/plans/big_files_refactor_plan.md` (god functions/duplication across the 7 biggest hand-written
files; **items 1+2 DONE 2026-07-02** — item 1: draw_panel split into `src/overlay_panel/` 8-file
section layout, render cpp 2150→~400 lines, in-game validated; item 2: shared marker gates, see the
DX-sweep entry below — remaining: 3 classify dedup, 4 diag quarantine, 5 icon_uv, 6 god functions,
7 grace-sprite design) and
`docs/plans/clang_only_toolchain_plan.md` (retire MSVC; USER DECISION reverses the same-day "MSVC
canonical" note in `docs/memory/tooling/build-toolchain-clang-xwin.md`). **Phase 0 update: the 3
`__try`-elision hazards (world_position per-frame probes + tutorial_popup init poll) are FIXED
(`5b80541`, built + deployed); still open = repo-wide `__try` classify pass, then Phases 1–2.**

## Feedback round 3 (2026-07-02, `feat/per-marker-native-glyphs`) — church glyph, fragment leaks, pin-RE diag

- **Churches drew circle:** the worldmap gfx has NO numeric `MENU_MAP_03` — the church glyph is
  NAME-keyed (`MENU_MAP_Church`). Fix: `map_point_name_alias()` in MapPointProvider (numeric
  resident → numeric disk → alias resident-by-name → alias disk-by-name). Checked the decompiled
  gfx symbol list: also missing numerics with NO known name = **15 (wells), 43 (Gelmir camp)** →
  those stay circle until a name/RE surfaces.
- **require_map_fragments leaks**, 3 root causes fixed:
  (1) QuestNpcLayer never set `fragment_flag` → quest pins ignored the gate; now derived from the
  pin's overworld tile (`quest_pin_fragment_flag`; UG groups stay ungated — tile ambiguous).
  (2) Off-shore islet tiles (Divine Towers) missed the ±1 neighbour fill → widened to ring ±2
  (`GetMapFlagFromTile`).
  (3) Unmappable AREAS (Roundtable m11_10, unconverted dungeon leftovers) returned 0 = "always
  shown" → `marker_fragment_flag` now returns a never-set sentinel flag (INT32_MAX) for a
  projected area outside {60,61,12,40-43}, so the existing renderer gate hides them while the
  toggle is on. Rune-piece leak most likely class (2)/(3) — recheck in-game.
- **Native pins still visible — RE diag added:** debug_logging now logs `[LANDMARKPIN] WMPP
  iconIds NOT ours (never suppressed): 41x191 67x26 80x…` at build. Expected residents: 41/67
  (bosses), 80 (graces), 83/84/85 (structural), 42 (sub-zones), 87 (VM requests), 0 (ERR arena).
  Anything the user still SEES beyond those → match its iconId here, then decide suppress/classify.
  NB: ERR also injects its OWN custom rows (stakes/pools/effigies at iconId 374+ etc.) — those are
  OUR old native-injection families, not WMPP vanilla pins.
- **Round 3 in-game results (user, 2026-07-02):** church glyph FIXED; fragment leaks mostly fixed;
  Roundtable items still visible — ACCEPTED as correct (its markers project through the m11 conv
  onto a Leyndell-fragment tile the player owns; the "always available since game start" reading
  fits the Roundtable anyway). `[LANDMARKPIN]` diag: `19x3 41x191 42x29 67x26 80x73 83x70 84x16
  85x16 87x8` — everything expected EXCEPT **iconId 19 (Windmill Pastures, 3 rows): missed in the
  first parity grouping pass** → added to `WorldTownVillage` (those were the user's remaining
  mystery pins).
- **Crowded-icon DX: hover FAN-OUT (spiderfy) — DONE + IN-GAME VALIDATED 2026-07-02
  (`feat/spiderfy`, entirely via the Phase 4 RPC loop — boot→save→map→hover all scripted, both
  states screenshot-verified, debugged live with a hot-reloaded debug-viz build).** Hovering a
  pile fans its members out (ring ≤12, archimedean spiral above, cap 40 + "+N more"), legibility
  backdrop disc, leg lines, per-member hover tooltips via the existing hover_test; sticky on the
  pile's tile cell key, closes when the cursor leaves fan extent + margin. Ini `cluster_spiderfy`
  (default on). Loop side-findings fixed on the way: RPC `mouse_move` needed the SetCursorPos
  TRAMPOLINE (SendInput absolute lands off-target under Wine) + a ±1px relative jiggle (a real
  mouse EVENT — otherwise the game re-warps the OS cursor onto its raw-input reticle and the
  placement lasts one frame); drivers should send mouse_move twice (rare warp race eats the
  first).

  **Spiderfy v2 (same day, user followups, both live-validated):** (1) zoomed-out GATE — no fan
  when one 256-unit map tile projects below 64 SCREEN px (zoom-range-agnostic criterion); pile
  tooltip still works there. (2) member DEDUP — fan shows ONE icon per distinct
  (name_id, category, icon) with an xN badge + "(xN in this pile)" tooltip line; a 52-pile
  rendered ~20 fanned entries with x2/x3/x5 badges instead of a 40-icon spiral.

## Phase 4 session findings (2026-07-02, spiderfy-v2/RPC-HUD run) — DX sweep + background-focus blocker

- **RPC command HUD shipped + live-validated:** bottom-right overlay feed of driver commands +
  results (host-drawn, `debug_rpc::recent_commands`, entries fade after ~6s; empty = draws
  nothing). New RPC input commands: `mouse_wheel <±notches>` (map zoom — validated), `type
  <text>` (focused ImGui field). **Wine/AZERTY key gotcha #2:** SendInput VK→scancode uses the
  US layout but the return scan→VK translation uses the HOST layout — so VK_A lands as 'q' on
  AZERTY. `type` can't compensate in-process (the game-side layout reads as US; VkKeyScanW is
  identity) — the DRIVER must send the QWERTY-position character (send "lqrvql" to type
  "larval"; a↔q, z↔w, m→';'). Proven live: search field filled "larval", 3 matches listed,
  result click panned the map + drew the search ring.
- **THE systemic background blocker found (answers the user's "stuck when unfocused" question
  for real): it is NOT the pause** (`paused=0` throughout) — **our own `g_has_focus` gate kills
  the keyboard poll AND mouse clicks when the game window loses focus**, so RPC UI driving
  (typing, clicking results) dies as soon as the user works elsewhere. SetForegroundWindow from
  in-process does not reliably steal X focus back under Wine. Next work: a dev-mode override
  (treat-as-focused while debug RPC is enabled — accept the "user keystrokes leak into the
  background game" tradeoff, it's symmetric with PauseTheGame's global keys) or real X-side
  focus forcing. Until then: leave the game window focused during scripted UI runs (world/map
  driving via `key`/menus works regardless — game reads its own input path).
- **Great Rune "(x2)" in search — NOT a bug (triaged with the user, 2026-07-02).** Every rune has
  TWO legit markers sharing the GoodsName: the boss-drop (live boss position — Mohg's is on the
  UNDERGROUND page, the tell that unmasked it) and the ACTIVATION site (Divine Tower — ring
  visually confirmed on the Divine Tower of Limgrave). Possible polish later: suffix the
  activation-site marker's tooltip ("(activation)") so search rows self-explain.
- **New bug (user, 2026-07-02): endless map pan after closing F1 with the cursor at a screen
  edge.** ER edge-pans while the OS cursor sits at the border; closing F1 leaves our virtual/OS
  cursor parked there → the map pans forever until the mouse moves. Likely fix: on F1 close (or
  map open?) recenter/step the cursor off the edge, or suppress edge-pan while the cursor is
  RPC-parked. Repro possibly easiest via RPC (mouse_move to edge + open_f1 0). Untriaged.
- **Quest-NPC altitude badges missing — FIXED (`fix/npc-altitude-badge`,** user question): the
  `entity_world_pos`/`g_entity_pos` path dropped MSB Y, so NPC pins never got the ▲/▼ badge.
  Y now threaded (EntityPos.wy from DiskEnemy/DiskCollectible.posY). In-game visual confirm
  pending (needs an NPC pin at a different altitude than the player).
- **Boot-time work — steps 1+2 DONE, in-game measured (2026-07-02,
  `perf/boot-io-profile-param-poll`).** (2) **param_poll fixed:** the 5005ms was NOT the poll
  interval (already 200ms) — it was the "honor `load_delay` as a MINIMUM total wait" sleep after
  the poll had already confirmed readiness. Ready now proceeds after a 250ms settle; `load_delay`
  kept only as the fallback minimum when the poll can't confirm (ini comment updated). Measured:
  `init.param_poll` **5005 ms → 250 ms**, "Initialization complete" **t+10.7s (was ~15s)**, SIG
  30/30, zero errors. (1) **Boot I/O profile shipped + run:** new `diag_boot_io` ([Debug], default
  off) widens the CreateFileW observer to ALL opens and arms it LIVE at the top of setup_mod (new
  `modutils::hook_now` — the normal queued path only goes live at enable_hooks, ~14s in, which is
  why the old observer couldn't profile boot). `[BOOTIO]` = +ms-since-arm, per-open latency,
  ok/FAIL, first 1500 lines then counted. **Findings (ERR/Proton run 2026-07-02): boot is NOT
  file-I/O bound** — 1500 opens totaled ~85ms of open latency, all sub-ms. Time lives in idle
  gaps between opens: t+0-7s engine init (sparse opens; a 1.5s gap around failing discord-ipc
  pipes, ~1.3s after the sd/cs_smain.bnk audio bank), regulation.bin opened t+7.1s then a 1.35s
  gap (decrypt/parse; from_params completes t+9.3s), then OUR MSB disk-loot burst (~964 MapStudio
  opens, t+10.4-11.2s ≈ 0.8s, matches the known prebuild cost). FAIL noise is benign (158×
  mods' log.txt probes, dxvk.conf, discord-ipc absent, PauseTheGame's pause_keybinds.ini).
  **Step 3 (only if wanted): targeted wait-hooks are the remaining tier — the data says candidates
  are the discord-ipc connect wait and the audio-bank gap, NOT file I/O; the regulation parse is
  engine work (dangerous tier, leave alone).** vkd3d shader-cache hygiene + SkipTheIntro remain
  the safe game-side levers.
- **DX sweep findings (multi-zoom):** (1) markers OUTSIDE the map canvas — **FIXED 2026-07-02
  (`refactor/marker-gates-clip-labels`, in-game validated via the RPC loop):** the engine's static
  full-map rect (view+0x350, now in `LiveView.mapMin/Max*`) is projected through the marker view →
  `PushClipRect` around the whole worldmap pass + the cull/pile-visibility tests use it (hover dies
  with the pixels). Validated on the morgott locate-clamp frame: the black void past the canvas
  edge went from dozens of floating icons to ZERO. NB the earlier "markers all over black at max
  zoom-out" screenshot was NOT this bug — that's undiscovered-canvas INSIDE the map rect with
  `require_map_fragments=false` (the deployed dev ini) — flipping it true live collapsed the spam
  to the discovered region (also a live proof of the new shared gate predicate). (2) pile label
  flood at far zoom — **FIXED same branch:** labels now gate on the fan's tile-px criterion
  (256-unit tile < 64 screen px → hidden; pile tooltip still names the location). Same session
  landed **big-files refactor item 2**: `marker_passes_gates()` (the 4 event-flag gates, worldmap +
  minimap now ONE predicate) + `refresh_player_world_y()`; icon-scale math deliberately NOT shared
  (minimap's base+clamp differs by design, see item-13 note in draw_minimap). Still open: (3) fan
  near screen edge overflows off-screen (no clamp/re-anchor — the canvas clip now trims it but
  doesn't re-anchor); (4) ER edge-pans when the cursor sits near a screen edge — scripted
  mouse_move near edges drifts the view between screenshots (driver beware, not a bug).
- **F2 (fog locate clamp): FULLY REPRODUCED, deterministic scripted recipe (2026-07-02).**
  With the map open + F1: type `godrick` (search works via RPC — no AZERTY remap needed for that
  word) → clicking "Godrick the Grafted (x1) - Overworld" pans fine (Stormveil is DISCOVERED on
  this save, ring visible = negative control). Then `morgott` → click "Morgott, the Omen King
  (x1) - Overworld" (Leyndell, deep fog) → **the pan CLAMPS at the edge of the pannable/revealed
  area: the map canvas ends, the rest of the screen is void, and the target is never centred (no
  search ring in view)** — exactly F2's description. Repro screenshot kept at
  `/tmp/mfg_rpc_test/f2_repro_morgott_clamp.bmp` (regenerate any time with the recipe above).
  Bonus: that same frame is the strongest capture of the "markers drawn OUTSIDE the map canvas"
  sweep finding — dozens of icons float on the black void past the map edge, over the day/night
  dial. Fix direction per the original F2 note: pan-OOB support (bypass/extend the panX/panZ
  clamp in the projection when centring on a locate target).

## SINGLE-DLL migration — profiles retired (2026-07-02, `feat/mapgenie-landmark-parity`)

User audit request confirmed profiles had ~no reason left: zero real per-profile DATA (the only
divergent bakes were EMPTY non-ERR stubs of ERR-only tables + `legacy_conv`, which is a pre-param-
residence fallback only — the live `goblin_legacy_fold` is primary). Implemented:
- `goblin::err_features_enabled()` (goblin_config.cpp) replaces compile-time `profile_is_vanilla()`:
  **ERR = `reforged.dll` loaded in-process** (GetModuleHandle at init; cached; logs `[PROFILE]`).
  Every ERR me3 profile lists reforged.dll BEFORE MapForGoblins so it's resident when our DllMain
  runs. v1 was a disk fingerprint (`menu/deploy/projects/ELDENRINGReforged`, in-game verified
  DETECTED on ERR) but disk presence false-positives when the SAME install hosts a vanilla.me3
  launch (ERR files on disk, not loaded) — the loaded-module check answers "what's RUNNING".
  ERR-only config force-disabled off-ERR at load, exactly like the old vanilla build.
- DELETED: `GENERATED_SUBDIR` CMake machinery, `MFG_VANILLA`/`MFG_PROFILE_VANILLA` defines,
  `tools/gen_nonerr_stubs.py`, `src/generated_{vanilla,erte,convergence}/` dirs (were gitignored),
  per-profile build dirs. `src/generated/` is THE single bake dir.
- build.bat `--vanilla/--convergence/--erte` KEPT but now only select packaging assets
  (README/gfx/SNAP_DIR) + offline pipeline data source; all profiles build/ship the same DLL from
  `build-err/`. inigen always emits the full ini (ERR entries included everywhere).
- `liveLootLabels` single default = false (vanilla package used to default true — changelog notes it).
- **BOTH SIDES VERIFIED in-game (2026-07-02), same install, same DLL:** ERR launch →
  `[PROFILE] ERR DETECTED (reforged.dll loaded in-process) — ERR-only config active`; vanilla me3
  launch (`internals/modengine/bin/me3 launch -g eldenring -e "<steam exe>" -p vanilla.me3`, runs
  fine from Linux) → `[PROFILE] ERR not detected — ERR-only config force-disabled`, 0 errors.
  Bonus mod-agnostic proof: the parity landmarks read the ACTIVE regulation — `[LANDMARKLIVE] 280`
  on vanilla vs 295 on ERR with coherent per-category diffs (Evergaols 10 vs 16 = ERR's added
  archery-challenge rows, Dungeons 63 vs 66, …). Still open: Windows `build.bat` + `build.bat
  snapshot` re-run (script edited; the validated snapshot flow predates this change). Detection
  risk: if a future ERR renames `reforged.dll`, add its new core native to the check.
- Baked-data plan impact: Phase A per-profile regen is MOOT; remaining bake work (name_regions/
  region_anchors → disk-MSB runtime, icon atlas) unchanged.

## Native-pin PARITY — 9 new landmark categories IMPLEMENTED + in-game VERIFIED (2026-07-02)

`feat/mapgenie-landmark-parity`: full audit of native WMPP pins (every family the game still draws
that we didn't re-draw) → 9 new categories via the same `build_live_landmarks` pass: Churches /
Ruins / Rises & Towers / Shacks / Forts / Castles / Towns & Villages / Colosseums / Unique Sites
(~167 rows; + iconId 62 Ashen Leyndell → LegacyDungeon). All WMPP pins are natively eventFlag-gated
(discovered-only) — we show all. Full audit table + skip rationale (42 sub-zones, 87 quest markers,
0 ERR-arena) in `docs/memory/features/mapgenie-landmark-categories.md`. ERR build DEPLOYED to
`~/Games/ERRv2.2.9.6/dll/offline/`. **Next: in-game verify** — grep `[LANDMARKLIVE]` (now a loop
log listing all 16 landmark cats; expect Churches ~28, Ruins ~38, RisesTowers ~21, Shacks ~24,
Forts 7, Castles 6, TownsVillages ~14, Colosseums 3, UniqueSites ~26), toggle a few in F1, then
merge. Colosseum got a `category_gpu_iconId` (24) native-glyph entry, rest = circle.
**VERIFIED in-game on ERR (2026-07-02): `[LANDMARKLIVE] built 295` — all 16 categories, every
count exactly as predicted; SIG 29/29 clean, 0 errors.** Ready to merge.

**Side finding (SUPERSEDED same day by the single-DLL migration above):** the non-ERR profile
bakes were stale/incomplete (vanilla missed the whole Group1/2 enum block → its build had been
broken since Group 1; erte/convergence missed whole files). Fixed first by syncing headers, then
made moot by deleting the per-profile dirs entirely.

## Native landmark-pin suppression — IMPLEMENTED, not yet in-game verified (2026-07-02)

`feat/native-landmark-pin-suppression`. **Path decision (user asked grace-path vs direct):** direct
areaNo=99 flip on the native WMPP rows — the grace SetTo draw-only hook exists ONLY to preserve the
fast-travel click, and landmark pins have no click action (tooltip-only, duplicated by our overlay);
the direct path needs zero new RE and reuses the proven eviction/restore pattern. Implementation:
registry in `goblin_section_visibility.cpp` (`reset/register/apply_native_landmark_suppression`,
`goblin_inject.hpp` facade, GOBLIN_RENDER_API-annotated for the hotreload split);
`build_live_landmarks` owns the lifecycle (reset RESTORES before clearing so a rebuild never bakes
a flipped 99 into markers/orig_area; the param iterator yields live-row references so `&row` is the
live table); the `menu_auto_toggle_loop` watcher re-applies on landmark-category / F10 master
flips (`take_native_landmark_dirty`); config `landmark_suppress_native` (World section, default
true, NOT ERR-only — vanilla native pins duplicate too). Per-category: only categories toggled ON
suppress their native rows; graces (80) / bosses (41/67) untouched. Flips take effect next map
open (same cadence as every areaNo owner). **CONFIRMED in-game (user, 2026-07-02)** with one gap:
minor-dungeon families (Caves / Hero's Graves) kept their native pins — their OVERWORLD pin comes
from the dist-view mark, so `areaNo_forDistViewMark` now flips to 99 alongside `areaNo` (both
saved/restored). **ALL CONFIRMED in-game (user, 2026-07-02): dungeon pins suppressed too.**

Same feedback round shipped 3 more fixes + a round 2 (deployed `e970671e`) — **ALL CONFIRMED in-game (user, 2026-07-02)**:
- **Collected black-disc bug:** the icon-legibility contrast disc under small icons kept full alpha
  when the icon dimmed as collected → looked uncollected. Disc alpha now follows the icon tint's
  alpha (`draw_legible_icon`, map_renderer.cpp).
- **"Map icons: ON/OFF" toast removed:** its only remaining trigger is the F1 menu's own master
  checkbox — announcing what the user just clicked was noise. `show_toggle_banner` kept (unused)
  for a future hotkey path.
- **Stakes/Pools glyph collision (v2 after user feedback on v1's swap):** both categories now draw
  the SAME native Marika-statue glyph (`MENU_MAP_89` — matches both in-world objects) differentiated
  by TINT (pools = multiplayer blue, stakes = warm gold — the game's own colour language).
  `MENU_MAP_21` reads as a lift platform (it IS the Grand Lift WMPP glyph — landmark GrandLift
  already used it) → given to `WorldElevator`, grey-tinted. Plumbing: `CATEGORY_GPU_ICONS` grew
  per-category `scale` (these 3 POI glyphs draw at 0.6× the 2.2 mapSymbolScale — at raw scale they
  dwarfed item dots, user-reported) and `tint` (ABGR multiplied into the draw tint via `mul_tint`,
  composes with collected-dim/boss-red). SB_MapCursor sheets re-checked with menu_tex_extract: no
  dedicated stake glyph exists natively; 21/89 rects eye-confirmed.
- **Spoiler-free coverage audit (user ask):** the only ITEM-identity leak outside `lot_backed` was
  Farmable Drops (non-lot, names the real notable drop) → `anonymous_marker(m)` = lot_backed ∨
  farmable now gates the "?" draw + tooltip. Deliberately NOT anonymized: pieces/kindling (identity
  IS the category), material nodes (fixed gather spots, randomizers don't touch them), world
  features. Enemy/EMEVD drops were already lot-backed → covered.

## Native-map landmark icon suppression — TRACKED (2026-07-02) → implemented same day, see above

The game still draws its OWN landmark pins on the native world map (Minor Erdtrees etc.), so our
new landmark categories (incl. the 5 that now use the native glyphs — DivineTower/Evergaol/
MinorErdtree/GrandLift/MiquellaCross) can visually DUPLICATE the native pins when toggled ON.
User decision: acceptable for now; the definitive fix is suppressing the native pins. Technical
precedent exists: native grace suppression (`goblin_grace_suppression.cpp`) and the areaNo=99
row-flip eviction used by section visibility — the same trick applied to the landmark
WorldMapPointParam rows (gated on "our category is ON", restore on OFF) should kill the native
pin without touching files. Scope when picked up: which iconIds to suppress = exactly the ones our
categories re-draw; keep ERR's own custom pins (boss/camp) untouched. **NB (2026-07-02): do this
AFTER the parity branch above merges — the suppression set is now the Group 1 + parity iconId
union (see the audit table in `docs/memory/features/mapgenie-landmark-categories.md`).**

## Group-2 Elevator MECHANISM SOLVED on Linux (2026-07-02) — implementation open

Full RE chain in `docs/re/linux_group2_prompt_binding_re_findings.md` (done entirely with in-DLL
probes + offline python — first end-to-end run of the Linux RE path). Net: recon's 5010 anchor was
LADDERS; real lever-lifts = ABP 8200-8501 "Pull/Push lever" -> ~55 ObjActParam rows (join col
+0x28) -> **MSB ObjAct EVENT section binds {asset entity, objActParamId}** => Elevator category is
a pure mod-agnostic disk parse (msbe: parse ObjAct events, filter param ids whose ABP text is a
lever, join asset position; refine by anim-id groups if gates over-capture). Smithing Table
SOLVED too (2026-07-02): [ASSETRADAR]+[ASSETCOUNT] -> model filter **AEG099_308**.
**BOTH IMPLEMENTED + MERGED to master + in-game verified (user, 2026-07-02)**: 54 Elevator markers
(AEG027_* lift family, positions near-perfect), 4 Smithing Tables. Branch
feat/mapgenie-group2-elevator-smithing merged; worktree cleaned. Group 2 remaining per the recon
doc: whatever the next MapGenie diff lists (see coverage_vs_mapgenie.py). Probes live in `src/goblin_param_scan.cpp`
([PARAMSCAN]/[EMEVDSCAN]/[ABPTEXT], debug_logging-gated).

## Linux runtime-RE path — investigate to stop the two-PC switch (2026-07-01, not started)

User pain: runtime RE is Windows-by-convention but the live game runs on the Linux box (Proton).
Options + trial plan in `docs/memory/tooling/linux-runtime-re-options.md` (default = in-DLL probes,
first trial = ceserver + CE GUI on the Proton pid). Related vision note (runtime modding framework,
NOT a plan): `docs/runtime_modding_framework_vision.md` — **capabilities-vs-vision GAP AUDIT done
2026-07-02: `docs/runtime_live_capabilities_audit.md`** (what's proven live, what's missing for a
full no-regulation.bin runtime mod, recommended battle order: FMG-inject + param_set_field quick
wins → param_add_rows pivot → items/events → file-resolution hook long-term).

## MapGenie category coverage — GROUP 1 MERGED; GROUP 2 (Portal) RE in progress (2026-07-01)

**GROUP 1 landmarks — MERGED to master + ERR in-game confirmed.** 6 `World -
DivineTowers/Evergaols/MinorErdtrees/GrandLifts/Dungeons/LegacyDungeons` built LIVE from
`WorldMapPointParam.iconId` (`build_live_landmarks()`, `src/worldmap/map_entry_layer.cpp`). `[LANDMARKLIVE]
built 114` (counts match off-disk), positions correct. Ghost = existing `WorldHostileNPC` (no work). All
default OFF. Circle-glyph followup outstanding (task chip spawned; see memory note).

**Miquella's Cross — added (branch `feat/mapgenie-group2-portal`).** DLC iconId 208 (13 rows) reuses the
same landmark pass — one enum entry + iconId branch. Built clean, deployed to `dll/offline/`. Restart ERR
+ grep `[LANDMARKLIVE]` (should now show `MiquellaCross 13`). Default OFF.

**Miquella's Cross + GROUP 1 landmarks — MERGED to master** (`5febe5f`).

**Portal (Group 2) — IMPLEMENTED on branch `feat/mapgenie-portal`, build-clean + deployed, NOT yet
in-game verified.** RE fully solved (`docs/re/windows_portal_aeg_re_findings.md`): a portal = an
**`AEG099_510`** sending-gate asset whose EntityID is bound as **arg[2] of EMEVD warp template
`90005605`** (the mod-agnostic "actually warps" signal; isolates ~23 real gates from the model's ~180
placements). Runtime pass: `msbe::parse_emevd_portal_gates` harvests the gate entity set from
`event/*.emevd` (shared with `load_emevd_world_feature_flags`), `build_disk_portal_markers` emits each
`AEG099_510` disk asset (aegRow 99510) in that set, dedup by entity. Label = PlaceName 6108700 "Sending
Gate". Default OFF.

**Next, in order:**
1. **In-game verify Portal (user).** Restart ERR, toggle `World - Portals`; grep `[LOOTDISK] ... Portal
   markers (AEG099_510 bound to warp template 90005605; N entities harvested, M LOD-dup collapsed)` —
   expect ~23 gates at real sending-gate spots (Four Belfries, Siofra, Leyndell, DLC, …). On pass, merge
   `feat/mapgenie-portal` to master.
2. **Landmark GLYPHS — DONE (2026-07-02, `feat/per-marker-native-glyphs`).** Single-iconId
   categories landed first (`category_gpu_iconId`); then the per-marker plumbing closed the rest:
   `Marker.map_icon_id` = the row's own WMPP iconId, set by `push_marker` for `Source::Live` only
   (bosses + landmarks — Baked/DiskMSB loot iconIds are item/massedit ids, never map glyphs, stay
   0); the renderer's MP_ID tier prefers it over the per-category id. Net: ALL 16 landmark
   categories (incl. the Dungeon/LegacyDungeon/parity unions) now draw their native per-site
   glyph, and on ERR-less installs bosses (41/67) get a native glyph too (ERR keeps the
   name-keyed `MENU_MAP_ERR_Boss` first). Still open: Portals have no known SB_MapCursor glyph
   (check `AEG099_510` sometime); the F1 "icons replaced" counter (`category_is_gpu_native`)
   doesn't know about per-marker resolution yet — cosmetic.
3. **Rest of GROUP 2 — recon done, NOT quick wins. See `docs/re/windows_group2_landscape_re_findings.md`.**
   Portal was the clean one *because* it had a harvestable EMEVD template (`90005605`). The rest do NOT:
   - **Elevator / Smithing Table are ObjAct-bound, not EMEVD-template-bound.** Anchors found:
     Smithing Table = ActionButtonText 7030 / ActionButtonParam **6250**; Elevator = "Descend" 3301 /
     ActionButtonParam **5010**. But those ABP ids do NOT co-occur with their assets in EMEVD args
     (`_probe_g2_actionbtn.py`), and no candidate AEG model matches the counts (`AEG099_630` = 235 broad
     placements, not 40 lifts). Next step = an **ObjActParam/AssetObjActParam** parse (ObjAct row whose
     button=5010/6250 → MSB assets carrying that ObjAct), a new param path per category — bigger than Portal.
   - **Hidden Passage** = hit-detected illusory walls, NO action button → no static signal (hardest).
   - **Wandering Mausoleum** = dynamic moving entity (hard). Martyr Effigy = already `WorldSummoningPools`;
     Dragon Shrine folds into Churches; Landmark(172) = editorial → skip.
   Recon artifacts: `tools/_probe_g2_templates.py` (template→model map), `_probe_g2_actionbtn.py`.
   NB: offline SoulsFormats probes now need temp files in the REPO dir (`os.path.abspath('.')`), not
   `%TEMP%` — Defender started denying `%TEMP%` writes mid-session (`WinError 5`).
4. **DONE — Farmable.** `WorldFarmableCollectible` ("Loot - Farmable Drops") shipped + ERR-verified
   (`[LOOTDISK] … 70 farmable-notable`): respawning enemy drops of notable mats (Smithing Stones /
   Golden Runes / Gloveworts), all-8-slots scan (notable item is in slot 2), ~70 markers, off by default.
   `WorldFarmableEnemy` DROPPED (floods, no boss filter). Tuning knobs (notable set / per-item icons /
   dedup granularity) documented in `docs/memory/features/mapgenie-landmark-categories.md`.

## ⚠️ IN PROGRESS — baked-data → runtime/disk migration (build_pipeline.py deletion is the END state)

Authoritative plan: **`docs/plans/baked_data_full_removal_plan.md`** (full inventory + 6-phase
sequencing). `build_pipeline.py` can NOT be deleted yet — it's the LAST step (Phase 5), not the
first: it still generates authored tables with no runtime source (`category_exceptions`,
`name_aliases`, `world_feature_models`, boss list, region tables), and `build.bat` calls it 3×.

**Landed so far:** Phase 1 (dead `goblin_enemy_names` bake removed, enemy-drop labels now resolve
mod-agnostic via `NpcParam.nameId → GetMessage`) — merged, deployed, in-game verified on ERR. Phase
A regen (all 4 profiles now `MAP_ENTRY_COUNT 0`, non-ERR DLLs rebuild clean via clang/ninja) — done
at the build level; **the in-game vanilla sanity-check is still the one open item** (mod-agnosticism
can only be proven in-game on a non-ERR install — deploy `build-vanilla/MapForGoblins.dll` to a
vanilla install to close this). `.MASSEDIT` chain proven fully dead (not just baked=0) — the 14
dead `generate_*_massedit` stages + `goblin_massedit.{cpp,hpp}` (orphaned, never compiled) deleted.
`item_icon_table.json`'s baked category-exception override deleted + recovered LIVE via
`EquipParamGoods.sortId` (`goods_sort_id()`, `+0x20`, cross-checked vs `goodsType@0x3e`) —
in-game verified on ERR (every split repopulated correctly). `grace_position_index` bake dropped
(was already offline-only/dead-to-DLL, no in-game check needed).

**Remaining follow-ups (not blocking, note for whoever does Phase 5):** Reforged item families +
a few DLC key items (goods ids 2008025-2008037) still fall to the "Loot - Crafting Materials"
catch-all (colliding in-cell sortIds) — need dedicated `sortId` rules or accept the catch-all. The
offline mirror (`tools/taxonomy_classifier.py`, `_validate_taxonomy_map.py`) still applies the
deleted exceptions — resync or retire. `generate_loot_massedit` still emits a now-unread
`.MASSEDIT` alongside its live JSON — drop that emission when convenient.
**DONE 2026-07-01:** the tracked dead `.MASSEDIT` artifacts (`data/massedit` + `data/massedit_generated`,
113 files / ~9.9 MB) were purged + gitignored — the chain is proven dead (no runtime/pipeline consumer;
category-test tools read `regulation.bin`/params + `items_database.json`, not `.MASSEDIT`, so this is
safe for the pending MapGenie-coverage work). `generate_loot_massedit` still (re)writes
`data/massedit_generated` locally each run — now ignored, not tracked.

`goblin_name_aliases_en` migrated + bake DELETED (2026-07-01, `feat/name-aliases-runtime`): F1 English
search aliases now resolve live from the active install's `msg/engus/*.msgbnd.dcx` off disk
(`src/worldmap/name_fmg_en.cpp`; two-pass loose-mod-wins / packed-vanilla-fills; FMG-v2 group-table
lookup keyed on marker `name_id`), replacing the ERR-frozen table. Offline 2754/2756 vs the old bake;
**IN-GAME verified on ERR (cross-language) AND vanilla** via the me3 CLI (`[NAMEEN] 9877` vanilla names,
≠ ERR's count → reads the active install — see `docs/memory/tooling/me3-cli-nonerr-launch.md`).

**Next session — pick the next baked artifact to eliminate** (easiest→hardest, per the plan's
inventory): `goblin_tile_tabs`/`goblin_major_regions` (real + identical on all 4 profiles → dedup into
`generated_shared/`, pure housekeeping); `goblin_region_anchors`/`goblin_name_regions` (assess vs
`WorldMapPointParam`+`WorldMapPlaceName`); the icon atlas (baked overlay atlas, the prime-directive
example, biggest remaining item — see "Baked-atlas removal" below for why it's deferred). The regen
pipeline + all 4 profile builds are runnable on the Windows box (clang/ninja), so the confirm loop
is doable there.

## Baked-atlas removal — audited, DEFERRED until native coverage widens

Gate before deleting the baked overlay atlas: prove which categories actually need it per mod. The
`[ICONTIER]` census tool (`map_renderer.cpp`) tags each icon's resolve tier (mp_name/mp_id/item/
rep/atlas/circle) and tallies per draw pass — kept in the tree for re-auditing. Last audit (ERR vs.
vanilla, unioned since the census is per-view): **~15 categories still depend on the baked atlas**
(vanilla: Hostile NPC, Spirit Springs, Stakes of Marika, Kindling Spirits, Interactables, Spiritspring
Hawks; ERR: Cookbooks, Crystal Tears, Consumables, Scadutree Fragments, Pots-n-Perfumes,
Bell-Bearings, Crafting Materials, Golden Runes Low, Rune Arcs; `World - Maps` hits `circle`, no
glyph anywhere) — **verdict: gate NOT passed, keep the atlas.** To re-run: open the map ~5s on ERR
and on a vanilla launch (`internals/modengine/./bin/me3 launch -g eldenring -e "<steam exe>" -p
vanilla.me3`), grep `[ICONTIER]` both times, diff — native/rep in ERR but atlas/circle in vanilla is
what removing the atlas would regress. Follow-up not yet investigated: several loot categories that
hit `atlas` instead of `item`/`rep` are lot-backed treasures where per-item resolution runs but
yields `item_icon_id=0` — a one-shot debug log in `push_marker` dumping `lotId`/`lotType` + the
resolved key would disambiguate "fundamentally iconId=0" from "a fixable lot-resolution gap".

## OPEN — deferred for later

- **Lag-spike hunt, real suspect `refresh.collected.*`.** `refresh.collected.read_wgm` shows spikes
  2-5ms (~30x its avg) in the `[SPIKE]` log despite supposedly already using a good lookup — not yet
  root-caused. Use the `[SPIKE]` warns + the bench spikes column to localize a hidden per-marker/
  per-frame O(n) cost or cache miss. (Cosmetic nit noticed along the way: the spike-ratio display
  divides by a near-zero baseline for quiet timers, e.g. "~600x its 0.01ms avg" — harmless but ugly,
  floor the avg in the ratio display whenever touching that code.)
- **Map-exit input softlock.** Root cause for the general "soft key lock at screen edge" turned out
  to be **external** — Deskflow (cursor-sharing KVM), not ER or this mod; fix is Deskflow-side, see
  `docs/re/windows_input_softlock_re_prompt.md`. The F1-mouse-dead half of the original report was a
  separate, already-fixed bug (see the Alt+Tab fix chain in `changelog.md`). Low priority to revisit.
- **Item-name localization (FR/EN) — DONE 2026-07-01, the bake is gone.** (Superseded the earlier
  "keep the bake" call.) The cross-language English alias no longer ships baked: it's read live from
  the active install's engus msgbnd off disk at init (`name_fmg_en.cpp`). The feared ~10 MB Oodle cost
  is a one-time init decompress (~1.5 MB retained index), not per-frame — acceptable, and it's the only
  mod-agnostic option (baked shipped ERR's names into every profile). See the baked-data section above.
- **Is non-ERR/vanilla a hard support target?** Still an open policy question — decides whether the
  baked atlas (and similar ERR-leaning bakes) can eventually go fully or must stay as a permanent
  vanilla-compat net.

## Known standing gotcha — double-DLL-load, not a code bug

If both `MapForGoblins.dll` (ERR build) and `MapForGoblins_vanilla.dll` are present in the mods
folder, BOTH load into the same process → doubled everything (double ImGui draw, double
MsgRepository PlaceName patch producing `?PlaceName?`, double hook installs, discovered-grace
markers appearing to vanish) — confirmed by log diff, not a real code bug in any of the affected
systems. **Fix (immediate):** only ship/deploy ONE DLL — the launcher should load `MapForGoblins.dll`
only (renaming it does NOT disable the mod; the launcher falls back to the stale `_vanilla.dll` — to
truly test mod-off, remove BOTH). **Fix (strategic): DONE 2026-07-02 — the single-DLL migration
retired the per-profile variants; new packages ship one `MapForGoblins.dll` for every install, so a
fresh install can no longer double-load. (Stale old `_vanilla.dll` files in EXISTING installs can
still double-load until removed — the named-mutex hardening below still has value.)** **Hardening TODO, not yet implemented:** a named-mutex check at init
(`CreateMutexW`) so a second instance bails before installing any hooks/ImGui/PlaceName patch and
shows a clear on-screen "double load detected, check your launcher config" banner instead of silent
double-draw corruption — currently the failure mode is confusing, not caught.

## Key findings / non-obvious facts (icon/glyph work)

- The 7 mod-added POI categories (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have NO ERR-custom glyph — their massedit iconIds
  (374+) point to glyphs absent from all current menu files (numeric glyphs cap at 261). Recover via
  a real `SB_MapCursor` glyph where one fits (e.g. summon→89), else circle.
- `MENU_MAP_ERR_*` (boss/grace) are ERR-only names; on non-ERR they won't resolve → circle if the
  baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
