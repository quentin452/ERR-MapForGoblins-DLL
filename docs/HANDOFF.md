# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.
Last updated: 2026-07-01t (`feat/inject-tutorial-popup` PR 4b of the goblin_inject.cpp
god-file split — IN-GAME CONFIRMED via log check, ready to merge — see directly below. Earlier
same day: PR 4a (world-position) IN-GAME CONFIRMED + MERGED (a crash occurred that session but is a known
pre-existing bug, NOT a regression); PR 3 (section-visibility) IN-GAME CONFIRMED + MERGED; PR 2
(item-classify) IN-GAME CONFIRMED + MERGED; PR 1 (icon-harvest) IN-GAME CONFIRMED +
MERGED; PR 0 — MERGED, in-game confirmed via log check; Phase A regen DONE on the Windows box
(parallel session) — all 4 profiles now MAP_ENTRY_COUNT 0,
non-ERR DLLs rebuilt clean via clang/ninja, Phase-1 enemy-name landmine closed at build level (see
the baked-data STATUS block below), dead `goblin_massedit.{cpp,hpp}` culled. Also scoped this day:
overlay-only hot-reload + AI Playwright RPC loop plan (not started); MSVC-canonical / clang-cl-alt
build toolchain policy formalized. Earlier same day: `feat/input-module` MERGED, F3 Alt+Tab
keyboard-dead bug FIXED + user-confirmed, minimap search-hit edge-clamp + search-hint fixes,
`feat/quest-npc-layer` + `feat/minimap-scale-cluster-search` MERGED.)

## RESUME HERE (2026-07-01t) — `feat/inject-tutorial-popup` PR 4b IN-GAME CONFIRMED, ready to merge

Branch `feat/inject-tutorial-popup` (forked from `master` after PR 0-3+4a merged), implementing
PR 4b. Extracted the WorldMapPointParam readiness probe, TutorialParam row injection (F10 banner
rows), and `world_map_open()` into new `src/goblin_tutorial_popup.cpp` (~354 lines, 3 spans:
`:89-134`+`:137-373`+`:433-463`). `world_map_open()` is a misplaced public utility (called from
`goblin_worldmap_probe.cpp`/`map_renderer.cpp`/`goblin_overlay.cpp`×2/`input_wndproc.cpp`) that sat
in the TutorialParam banner section by proximity only. **Scope narrower than the plan's original
guess**: the toast-method A/B experiment (`g_toast_method`/`TOAST_METHOD_NAMES`/
`TOAST_METHOD_COUNT`) and toast-delivery internals (`show_tutorial_popup_trampoline`/
`seh_dispatch_toast`) STAY in `goblin_inject.cpp` — called ONLY by `show_toggle_banner`/
`seh_fire_trampoline` (PR 4d's toast subsystem), moving them would just relocate half that hub for
zero coupling reduction. Side finding (not fixed, pure relocation): the comment directly above
`world_map_open()` in the original file describes `show_toggle_banner`'s toast-firing behavior, not
`world_map_open` — a pre-existing stale/misplaced comment, moved as-is. Declarations in
`goblin_inject.hpp` unchanged (facade kept). Builds clean via clang-cl+xwin, deployed to
`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, prior DLL backed up as
`.bak-pre-tutorial-popup`). **IN-GAME CONFIRMED 2026-07-01 20:16 via log check**: fresh
`NEW SESSION`, `[SIG]` 29/29 PASS, `[TOAST] TutorialParam expanded: 11057 -> 11068 rows` (the moved
`inject_tutorial_popup_rows()` ran correctly), `[OVERLAY] world_map_open CSMenuMan_slot=0x...` +
repeated `CSMenuMan+0xCD changed -> 0/3/7/3/0` over ~40s (the moved `world_map_open()` resolving and
driving map open/close correctly), no error/exception. Same known pre-existing unrelated
`eldenring.exe` crash fired again this session (same `+0x1EB9999` offset, zero `MapForGoblins.dll`
frames) — not a regression. **PR 4b is done and verified — merged to `master`.** PR 4c remains
unstarted, 4d is the intended final resting state of `goblin_inject.cpp`.

## OLDER RESUME (2026-07-01r) — `feat/inject-world-position` PR 4a IN-GAME CONFIRMED, MERGED

PR 4 (the final cleanup pass) got its own scoping audit first, per the plan's own rule — every
prior PR found the plan's pre-PR0 guesses about "what's left" wrong somewhere, so PR 4 needed a
fresh full-file read of the post-PR3 ~1636-line `goblin_inject.cpp` rather than trusting the old
section list. Findings: scoped into **4a** (world-position/grace-data, DONE below), **4b**
(TutorialParam + toast-popup delivery + the misplaced `world_map_open()` — not started), **4c**
(native/DRAW-ONLY grace suppression, already known-independent — not started), and **4d**
(~260 lines that should STAY in `goblin_inject.cpp` permanently: the event-flag core `orp_flag_set`
— already a shared-accessor hub 2 other files depend on — plus `menu_auto_toggle_loop`, a generic
watcher touching state across 3 split files; moving this would just relocate the hub, not reduce
coupling). Also found: `goblin::inject_map_entries()` has NO implementation anywhere in the
codebase (likely orphaned by the parallel baked-data-removal session's native-injection
retirement) — `apply_flag_or_pairs()`'s entire body is a guaranteed no-op as a result, flagged in
the plan doc, not fixed (pure relocation is this plan's whole point).

Branch `feat/inject-world-position` (forked from `master` after PR 0-3 merged), implementing PR 4a.
Extracted the dungeon→overworld legacy-fold projection, grace anchors + region/cluster naming,
live WorldChrMan player-position resolve, live grace capture, and the `marker_fragment_flag`/
`marker_world_pos` wrappers into new `src/goblin_world_position.cpp` (~793 lines, 2 spans:
`:60-176` + `:187-826`) — fully self-contained per audit (confirmed via a file-wide grep, not just
a local read), zero cross-file accessors needed. 2 dead-but-still-referenced globals
(`g_injected_row_ptrs`/`g_lot_backed_set`) sit between the two spans and stay behind since
`apply_flag_or_pairs`/`injected_row_ptrs()` (the dead subsystem above) still reference them.
Declarations in `goblin_inject.hpp` unchanged (facade kept). Builds clean via clang-cl+xwin,
deployed to `~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, prior DLL backed up
as `.bak-pre-world-position`). **IN-GAME CONFIRMED 2026-07-01 20:06 via log check**: fresh
`NEW SESSION`, `[SIG]` 29/29 PASS, grace sprites/icons resolving correctly (`[GRACE-SPRITE]
'MENU_MAP_01_Bonfire' LOCKED`, `[GRACEUNDISC]` diag firing) — exercises the moved grace-anchor
code. **A crash DID occur ~39s into this session (`MapForGoblins_crash_348.txt`) but it's a known
pre-existing bug, NOT a regression**: fault is inside `eldenring.exe` itself (zero stack frames
touch `MapForGoblins.dll`), at module offset `+0x1EB9999` — the EXACT SAME offset as ~13 other
crash dumps in this same log dir going back to 2026-06-28, days before any of today's refactor
work (matches the already-documented "intermittent render-thread race", see
`docs/memory/bugs/mapforgoblins-map-open-freeze.md`). **PR 4a is done and verified — merged to
`master`.** PRs 4b/4c remain unstarted, 4d is the intended final resting state of
`goblin_inject.cpp`.

## OLDER RESUME (2026-07-01p) — `feat/inject-section-visibility` PR 3 IN-GAME CONFIRMED, MERGED

Branch `feat/inject-section-visibility` (forked from `master` after PR 0+1+2 merged), implementing
PR 3 of `docs/plans/goblin_inject_refactor_plan.md`. Extracted per-section visibility + marker-
clustering into new `src/goblin_section_visibility.cpp` (~675 lines) — 8 non-contiguous spans (not
a clean sed range like PR 0/1/2): visibility globals + `is_section_hidden_ptr` (`:187-428`),
cluster-config helpers + `seed_runtime_gates` (`:1140-1258`), the visibility/clustering HALF of
"Overlay control API" (3 sub-spans within `:1736-1955`, leaving save/reset requests + the toast
subsystem + `injected_row_ptrs` behind — those are unrelated concerns that happened to share the
same banner section), and the per-category census (`:2118-2172`). Grace anchors turned out NOT
coupled to visibility/clustering (own state, zero overlap) — correctly left for PR 4, the plan's
original table wrongly lumped them in by textual proximity. 2 build errors the audit's boundary
list didn't catch, both caught immediately by the compiler: toast-queue globals
(`g_toast_mtx`/`g_toast_queue`/etc.) sat inside the visibility span by proximity only — moved back
to `goblin_inject.cpp` for `enqueue_toast`/`menu_auto_toggle_loop` (stay-behind toast subsystem);
`persist_settings()` lost its `static` (only called from `menu_auto_toggle_loop`, staying behind)
and joined `goblin_inject_shared.hpp` alongside 2 new accessors (`icons_user_disabled()`/
`take_section_apply_req()`) — same established PR 0/1 accessor-header pattern. Declarations in
`goblin_inject.hpp` unchanged (facade kept). Builds clean via clang-cl+xwin, deployed to
`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, prior DLL backed up as
`.bak-pre-section-visibility`). **IN-GAME CONFIRMED 2026-07-01 19:54 via log check**: fresh
`NEW SESSION`, `[SIG]` 29/29 PASS, ~2 minutes of real play — world map opened/closed multiple times
(`CSMenuMan+0xCD` 0→3→7→3→0), `refresh.category_census` running repeatedly, `render.worldmap.clusters`
bench fired (32 samples) — directly exercises the moved census/clustering-config machinery. No
crash/error/exception. **PR 3 is done and verified — merged to `master`.** PR 4 of the same plan
remains unstarted.

## OLDER RESUME (2026-07-01n) — `feat/inject-item-classify` PR 2 IN-GAME CONFIRMED, MERGED

Branch `feat/inject-item-classify` (forked from `master` after PR 0+1 merged), implementing PR 2
of `docs/plans/goblin_inject_refactor_plan.md`. Extracted taxonomy-based item/loot classification
into new `src/goblin_item_classify.cpp` (~430 lines) — distinct from PR 0's ItemLotParam-ROW-based
loot resolve, this classifies by ER's own (goodsType, sortGroupId) taxonomy. **Non-contiguous in
the original file**: taxonomy helpers forward-declared/defined at `:1070-1167`, the actual public
API at `:2299-2607` (~1130 unrelated lines between them — TutorialParam injection, native grace
suppression, overlay control API, kill indicators). Both spans moved together (only depend on each
other, zero coupling to the gap) — no accessor header needed, the cleanest PR yet despite the
non-contiguous shape. Scope grew from the plan's 5-function estimate to 9 — `aeg_is_gather`,
`npc_item_lot_enemy`, `item_real_icon_id` were physically wedged into the same span sharing
structs (`RawAegRow`/`RawNpcRow`/`RawEquipRow`) with the 5 named ones, couldn't be split out
separately. Declarations in `goblin_inject.hpp` unchanged (facade kept). Builds clean via
clang-cl+xwin, deployed to `~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, prior DLL backed up as
`.bak-pre-item-classify`). **IN-GAME CONFIRMED 2026-07-01 19:37 via log check**: fresh `NEW SESSION`
19s after deploy, `[SIG]` 29/29 PASS, no crash/error, and `[ITEMCLASS]` census (the moved
`classify_item_live` chain) producing correct live output (e.g. `key=500008975 cat="Quest -
Progression"`) — directly exercises the whole moved taxonomy path. **PR 2 is done and verified —
merged to `master`.** PRs 3-4 of the same plan remain unstarted.

## OLDER RESUME (2026-07-01l) — `feat/inject-icon-harvest` PR 1 IN-GAME CONFIRMED, MERGED

Branch `feat/inject-icon-harvest` (forked from `master` after PR 0 merged), implementing PR 1 of
`docs/plans/goblin_inject_refactor_plan.md`. Extracted the icon-texture harvest/GPU-icon-registry
block (`:1688-3939`, ~2252 lines — bigger than the plan's original ~1500-1600 estimate) out of
`goblin_inject.cpp` into new `src/goblin_icon_harvest.cpp`. Audit (forked, same rigor as PR 0)
found exactly ONE cross-boundary coupling: `goblin_inject.cpp`'s `warp_pin_detour` (native
discovered-grace pin suppression, stays behind) needs `icon_rpm_i32`/`icon_rpm_ptr` — fixed with a
local duplicate in `goblin_inject.cpp`, same per-file-copy convention PR 0 established. 30 public
functions declared in `goblin_inject.hpp` unchanged (facade kept, zero call-site churn across
`goblin_overlay.cpp`/`goblin_worldmap_probe.cpp`/`goblin_markers.cpp`/`goblin_messages.cpp`/
`dllmain.cpp`/`worldmap/*`). Needed 2 include fixes not caught by the audit (missing `<set>`,
missing `from/params.hpp` + `goblin_messages.hpp` for `lookup_text_utf8`) — build caught these
immediately, nothing subtle. Builds clean via clang-cl+xwin, deployed to
`~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll` (md5-verified, prior DLL backed up as
`.bak-pre-icon-harvest`). **IN-GAME CONFIRMED 2026-07-01 19:28 via log check**: fresh `NEW SESSION`
22s after deploy, `[SIG]` 29/29 PASS, `[GRACE-SPRITE] 'MENU_MAP_01_Bonfire' LOCKED ...` (exercises
the moved icon-harvest code directly), no crash/error/exception. **PR 1 is done and verified —
merged to `master`.** PRs 2-4 of the same plan remain unstarted.

## OLDER RESUME (2026-07-01i) — `feat/inject-module` PR 0 done + in-game CONFIRMED, MERGED

Branch `feat/inject-module` (forked from `master`), implementing PR 0 of
`docs/plans/goblin_inject_refactor_plan.md`. Extracted `LotReader` + the live-persistence
classifier + its 4 consumer functions (`resolve_loot_flag`, `resolve_loot_item_textid`,
`lot_row_in_table`, `lot_item_count`) + `diag_loot_flags` out of `goblin_inject.cpp` into new
`src/goblin_loot_resolve.cpp`, with a 3-accessor shared header (`goblin_inject_shared.hpp`) for
the one genuine cross-file dependency (`orp_flag_set`, which stays in `goblin_inject.cpp` since
other sections there still need it). Pure relocation, no logic changes. Builds clean via
clang-cl+xwin on this Linux box and DEPLOYED to `~/Games/ERRv2.2.9.6/dll/offline/MapForGoblins.dll`
(md5-verified, prior deployed DLL backed up as `.bak-pre-inject-module`). **IN-GAME CONFIRMED
2026-07-01 19:20 via log check**: fresh `NEW SESSION` right after deploy, `[SIG]` health check
29/29 PASS (incl. `IS_EVENT_FLAG`/`EVENT_FLAG_MAN_SLOT`, the AOBs the new shared-header accessor
depends on), `diag_loot_flags` (the moved function) producing correct-looking live `[LOOTDIAG]`
output, no crash dump/error/exception after session start. **PR 0 is done, verified, and MERGED to
`master`** (fast-forward, branch `feat/inject-module` deleted post-merge). Plan doc updated in
place with the PR-0 findings (LotReader turned out NOT to need the separate "shared extraction
first" step the plan originally called for — see the plan's own "PR 0 finding" note). Next: PR 1
(icon-texture harvest/GPU registry, `:1688-3939` in the current file) — coupling audit in progress.

## NEW PLAN (2026-07-01h) — overlay-only hot-reload + Route B AI Playwright loop, scoped not started

`docs/plans/overlay_hot_reload_playwright_plan.md` — <user> wants to reload ONLY the ImGui overlay
draw code live (no ERR restart) so an AI can screenshot the real running game via the already-
proposed Route B debug RPC (`docs/memory/tooling/overlay-test-harness.md`), spot a DX or functional
bug in minimap/worldmap/icons, fix it, hot-reload just that piece, and re-verify — a tight iterate
loop against the real game (not the offline Route A mock harness, which is unaffected). 4 phases:
(1) extract `draw_panel`/`draw_worldmap_markers`/`draw_minimap_hud` out of `goblin_overlay.cpp`
behind a context-struct interface, no reload yet — pure refactor, verify zero behavior change; (2)
split into its own DLL loaded via `LoadLibrary` + dev-only file-watch reload (ImGui-context-sharing
and thread-safety are the real risks, not the LoadLibrary mechanics); (3) greenfield Route B RPC
(no IPC exists in `src/` today) with commands incl. new `reload_overlay`; (4) wire the actual AI
loop. `dx-bugs-backlog` items 11/12 are explicitly NOT good first loop targets (11 = deploy-hygiene
double-DLL issue not a code bug, 12 already fixed) — pick a live backlog item once Phases 1–3 land.
Windows-only (LoadLibrary/MinHook). Not started — fork an implementation branch when work begins,
per `AGENTS.md`.

## ⚠️ IN PROGRESS (2026-07-01h) — baked-data → runtime/disk migration (build_pipeline.py deletion is the END state)

**STATUS 2026-07-01h (the baked-data agent — Phases 1 + A landed, see below):** the
"supprimer build_pipeline.py, on a déjà une path disk" instruction was scoped into a real plan and
Phase 1 landed. Key correction to the original framing: **`build_pipeline.py` can NOT be deleted now
— it is the LAST step (Phase 5), not the first.** It still generates the authored tables compiled
into the DLL (`category_exceptions`, `name_aliases`, `world_feature_models`, boss list, region
tables) that have no runtime source yet, and `build.bat` calls it 3×. Deleting it now breaks every
build. Authoritative plan: **`docs/plans/baked_data_full_removal_plan.md`** (full inventory + 6-phase
sequencing + the post-Phase-1 findings that reframe Phase 2). What landed / what's next:

- **DONE — Phase 1 (enemy names), merged `5b24392` + deployed + in-game-verified on ERR.** The
  `goblin_enemy_names` +900M TutorialTitle bake was DEAD (fed the retired `MAP_ENTRIES`;
  `MAP_ENTRY_COUNT==0`, log `[COVERAGE] baked=0`). Removed it (injection, generated files, CMake
  entries, `generate_data.py` emitter, pipeline output ref, `extract_enemy_names_i18n.py`). Enemy-drop
  labels resolve mod-agnostic via `npcParamId → NpcParam.nameId → GetMessage(NpcName)` (shipped
  hostile-NPC path). Log after deploy: 0 errors, GETMESSAGE PASS, SANITIZE cleared 0. Offline
  feasibility tool: `tools/verify_enemy_name_runtime.py`.
- **DONE — Phase A regen (2026-07-01h, commit `fe8d28c` + this branch `phaseA-regen-nonerr`).** The
  Phase-1 landmine (stale non-ERR bakes `generated_vanilla` **6916** / `generated_convergence` **7448**
  referencing the deleted +900M resolver) is **closed at the build level**: reran
  `build_pipeline.py --profile {vanilla,convergence,erte}` → **all 4 profiles now MAP_ENTRY_COUNT 0**,
  and all 3 non-ERR DLLs rebuild+link clean (`build-{vanilla,convergence,erte}/MapForGoblins.dll`, ~4 MB,
  clang/ninja). `generated_*` + `data/<profile>` are gitignored local artifacts, so the only committed
  change is the pipeline fix. **STILL PENDING (user, needs the game): the in-game vanilla sanity-check**
  — mod-agnosticism (enemy/loot labels resolving live on a non-ERR install) can ONLY be proven in-game
  on a non-ERR profile; ERR passing is necessary, not sufficient (guardrail). Deploy the vanilla DLL
  from `build-vanilla/MapForGoblins.dll` to a vanilla install to close this + the long-open
  "vanilla+DLC verify" item.
  - **Machine-capability correction:** the old "blocked on a machine that can build non-ERR — 2026-07-01
    dev machine can't (CMake configure fails)" note was the Linux/MSVC framing. THIS Windows box builds
    all 4 profiles via **clang-cl + xwin + ninja** (`build-<p>/`, see
    `docs/memory/tooling/build-toolchain-clang-xwin.md`), NOT `build.bat`/MSVC (no VS installed here).
    The real blocker was that the **regen** had never run on Windows — it crashed on the SoulsFormats
    temp-file unlink because `tools/sitecustomize.py` (the tolerant-unlink safety net) never auto-loaded.
    Fixed in `build_pipeline.py` (`fe8d28c`): it now puts `tools/` on child `PYTHONPATH` so `site`
    imports sitecustomize. This also fixes `build.bat`'s pipeline stages on Windows.
- **`.MASSEDIT` chain PROVEN DEAD (not just baked=0), `239b1ec`:** nothing reads `.MASSEDIT` anywhere —
  the DLL loader `goblin_massedit.{cpp,hpp}` was orphaned (not in CMake, never compiled) and is now
  deleted; `generate_data`'s `massedit_dir` is a dead param (map_data stubbed unconditionally); the only
  pipeline reader `relocating_boss_fix` just edits MASSEDIT files that feed the empty stub. So the ~14
  `generate_*_massedit` stages produce output read by nothing.
- **NEXT PRs (evidence-based, in the plan):** (1) ~~Phase A regen~~ DONE at build level; only the
  **vanilla in-game sanity-check remains (user)**. (2) ~~cull the dead `generate_*_massedit` stages~~
  **DONE — branch `pr2-dead-massedit-cull`** (`a980746` refresh stale aliases bake, `1be1be4` the cull).
  Removed the 14 MASSEDIT generators + their 5 now-orphaned extractors + `relocating_boss_fix` + the
  `massedit_dir` param; kept `generate_loot_massedit` (JSON half feeds compiled tables). **Confirmed dead
  OFFLINE** (stronger than the planned log-diff): regen ERR before/after with `data/massedit_generated`
  wiped → `src/generated` BYTE-IDENTICAL, ERR DLL rebuilds clean. User game-checks ERR later.
  **Follow-ups left for a Phase-5 sweep:** the ~19 orphaned generator *scripts* (`generate_stakes.py` …)
  are still on disk; the committed dead `data/massedit_generated/*.MASSEDIT` artifacts (tracked, 71 files,
  a mix of live-loot + dead-stage output — some regenerate non-deterministically) are untouched; and
  `generate_loot_massedit` still *emits* dead `.MASSEDIT` alongside its live JSON (drop that emission when
  convenient). (3) **← IMMEDIATE NEXT:** migrate `item_icon_table.json` (ERR-frozen placed-item set) to a
  runtime item enumeration. (4) Phase 5 retire the pipeline + build.bat call sites + README.
  - Note: the regen pipeline + all 4 profile builds are runnable on THIS Windows box (clang/ninja +
    `fe8d28c`), so the confirm loop for (3)/(4) is doable here.

**Findings gathered before handing off (historical context; Phase 1 above now acted on):**
- `build_pipeline.py` (526 lines) is called from **3 places in `build.bat`**: the `:generate`
  label (`build.bat:93`), `:snapshot` (`build.bat:135`), and `:release` (`build.bat:183`). Simply
  `rm`-ing the file without touching these leaves `build.bat` broken (file-not-found) for every
  profile, including ERR's own daily-driver build — this is NOT scoped only to the
  vanilla/erte/convergence regen path `generated_data_removal_plan.md` Phase A describes.
- `build_pipeline.py` orchestrates ~30 individual generator/extractor scripts (grep `script=` in
  the file — `extract_goods_categories.py`, `extract_itemlot_csv.py`, `generate_boss_list.py`,
  `generate_data.py` itself, `generate_world_feature_models.py`, etc.), not just the marker-position
  bake `generated_data_removal_plan.md` targets. Per that plan's own audit, SOME of what it
  produces is genuinely non-derivable-from-disk authored content on non-ERR profiles (enemy_names
  — no Codex-equivalent live source; category_exceptions/name_aliases_en — curated dicts, no
  param/MSB source at all) — full removal of the orchestrator has a wider blast radius than just
  the map-data stub this plan was originally about. Whoever finishes this migration should
  reconcile `docs/plans/generated_data_removal_plan.md` against whatever actually survives (it may
  need a rewrite or a "superseded by direct deletion" status update once this lands).
- User's stated reasoning: the live-DiskMSB/on-disk read path already covers what this baked
  pipeline was for, so keeping the orchestrator (and the profile-specific baked tables it
  produces) is no longer justified. This is a stronger claim than what
  `generated_data_removal_plan.md` itself verified (that plan found several tables ARE genuinely
  permanent/non-disk-derivable) — if the other agent's migration disagrees with that plan's
  conclusions, that's an intentional user call, not a bug to "fix" back.
- **Next session / whoever picks this up:** check `git log`/`git status` for what the parallel
  agent actually did, verify `build.bat` still works (or was intentionally retired/replaced) for
  at least the ERR profile, and update `docs/plans/generated_data_removal_plan.md` +
  `docs/plans/README.md` + `docs/memory/tooling/mapforgoblins-pipeline-setup.md` to match new
  reality (all three currently describe `build_pipeline.py` as the active pipeline).

## RESUME HERE (2026-07-01f) — input-module refactor + F3 keyboard fix + 2 minimap/search DX fixes, all landed + confirmed

- **`feat/input-module` merged to master** (6 commits, fast-forward, no conflicts). All 5 input
  hook groups (DirectInput, XInput, cursor, raw input, wndproc) moved out of `goblin_overlay.cpp`
  into `src/input/*.cpp` — see `docs/memory/tooling/input-hooks.md` for the file map + where to
  add the next `[XXXDIAG]` counter. Pure relocation + thin accessor plumbing, no logic changes;
  user confirmed working live in-game.
- **F3 (Alt+Tab keyboard-dead) — ✅ FIXED + user-confirmed live (`90b3e2b`).** Deterministic
  repro (type works → Alt+Tab away+back → typing permanently dead) log-confirmed
  (`MapForGoblins.log` 17:33:17-24: `wm_char/sec`/`wm_keydown/sec` go to 0 forever right after
  `WM_SETFOCUS` while `WantCaptureKeyboard`/`WantTextInput` stay true — legacy keyboard messages
  simply stop arriving, same `RIDEV_NOLEGACY` family as the earlier Proton mouse-click fix). Fix:
  `src/input/input_keyboard_poll.cpp` (`GetAsyncKeyState` + `ToUnicodeEx`) is now the SOLE
  keyboard-text source while the menu is open; `input_wndproc.cpp` no longer forwards
  `WM_CHAR`/`WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` to ImGui at all (avoids
  double-typing while legacy messages still worked, pre-Alt+Tab). **Known limitation: no OS
  auto-repeat emulation** — a held key types once per physical press, not on a timer. Flag if
  reported as annoying.
- **Minimap search-hit edge-clamp (`5a4ea4b`) + search-hint text fix (`6b97194`)** — both landed,
  not yet explicitly re-confirmed live but low-risk/mechanical: (1) an item-search target outside
  the minimap's radius used to just vanish (only in-range items ever showed) — now clamped to the
  HUD edge along its true direction, like an off-screen objective indicator; (2) the item-search
  hint text always said "open the world map to locate them" even with the minimap on and already
  showing the hit — now says "ringed on the minimap" in that case.

## Session recap (2026-07-01) — MapGenie coverage RE: FULLY DISCHARGED (Part A + Tiers 2/3/4 verified)

- Started executing `docs/re/windows_mapgenie_category_coverage_re_prompt.md` (verify-only, no impl
  ahead of `generated_data_removal_plan.md` Phase B). Key method finding: params are baked verbatim
  into `regulation.bin` and NOT streamed — reading it off disk via SoulsFormats is value-identical to
  the live `from::params::get_param` path, so **no running game / memory attach needed** for any
  param-based tier. New tool: `tools/verify_disablerespawn.py`.
- **Part A(a) `WorldFarmableEnemy` = `NpcParam.disableRespawn` — VERIFIED, partially wrong.** `dr=1`
  reliably = one-time/not-farmable (invaders, quest NPCs, prop dummies). But `dr=0` does NOT mean
  farmable: the fog-gated main bosses (Rennala, Draconic Tree Sentinel, …) read `0` in BOTH vanilla
  and ERR — their non-respawn is enforced by boss-defeat event flags, not this field. So the gate is
  `dr==0 AND already-classified-non-boss` (reuse existing `WorldBosses` classification; do NOT rely on
  `dr==1` to exclude bosses). Findings + citations: `docs/re/windows_mapgenie_category_coverage_re_findings.md`;
  plan + Open-Q #2 updated in place.
- **Part A(b) `WorldFarmableCollectible` = ItemLotParam flag — VERIFIED, field guess wrong.** Plan said
  `getItemFlagId01/02/03`; actually the per-slot `01..08` fields are ALWAYS 0 (unused), and the
  authoritative field is the single master `getItemFlagId` (already read live @ +0x80 in
  `resolve_loot_flag`, `goblin_inject.cpp:4720`). Polarity: `==0` = farmable, `!=0` = tracked — but
  nonzero≠one-time, some are *repeatable* (`flag_is_repeatable`, `:4679`). Gate:
  `getItemFlagId==0 OR flag_is_repeatable(...)`. Zero new plumbing (field + helper already exist). Tool:
  `tools/verify_farmable_collectible.py`. Both Part A items now done; findings + plan updated in place.
- **Tier 2 `WorldMapPointParam.iconId` landmarks — VERIFIED, big finding.** The plan's Tier-2 "numbers"
  were MapGenie pin-COUNTS, not iconIds. Real iconIds (stable vanilla↔ERR, `tools/verify_worldmap_iconids.py`):
  Divine Tower=**23**, Evergaol=**9**, Minor Erdtree=**30**, Grand Lift=**21**; "Dungeon"=union of typed
  icons {4 catacombs,13 caves,14 tunnels,16 hero graves,15 wells,230/231/234}; "Legacy Dungeon"=per-site
  unique icons {50,51,55,56,58,59,60,61,66,210,211,213,218}. BUT ~half the requested categories are NOT
  in WorldMapPointParam at all (Smithing Table, Martyr Effigy, Stone Cairn, Hidden Passage, Portal,
  Wandering Mausoleum, Dragon Shrine, Landmark) — they're AEG/MSB interactables, summoning pools, dynamic
  entities, or catch-alls → different source, must NOT be scoped as WMPP work. ERR only decorates the
  point *text* (boss status); iconId itself unchanged → mod-agnostic. Findings + plan updated in place.
- **Tier 3 `NpcParam` teamType/npcType — VERIFIED, hypothesis mostly collapses.** `teamType` is a
  per-ROW combat allegiance (each NPC spans 3–6 teams across state rows), `npcType` is ~all-0 (==1 set is
  4 named rows) — neither is a per-NPC category key. Only clean signal: **Ghost = invader = teamType∈
  {24,27} ∧ nameId>0 = the ALREADY-SHIPPED WorldHostileNPC** (`goblin_inject.hpp:360`), no new work. The
  other 5 need non-teamType sources: Merchant→ShopLineupParam, Character→existing QuestNpcLayer,
  Trainer→1 hand-ID'd NPC, Elite Enemy/Enemy→`datamine_enemy_notability.py`. Tool:
  `tools/verify_npc_teamtype.py`.
- **Tier 4 — INVESTIGATED (no tool needed).** Lore (6) + Miscellaneous (9) are MapGenie "Other (guide
  annotations)" = human editorial pins with NO game-data source; the only source (MapGenie) is rejected
  (ToS + coord-space). Recommend dropping as permanently-uncovered. Quest (7) already covered (mod draws
  73 Quest-Progression + 7 Seedbed-Curses + 71 runtime QuestNpcLayer pins). No code.
- **RE BRIEF DISCHARGED.** All tiers turned from hypothesis → cited fact; every correct mechanism already
  exists in shipped code. Implementation still gated on `generated_data_removal_plan.md` Phase B (plan's
  sequencing). Findings doc has the full per-tier writeup + an "Overall status" summary; plan + all 3
  Open-Questions updated in place. 4 `tools/verify_*.py` added.

## Session recap (2026-07-01) — minimap branch: cursor tracking rebuilt (4 rounds), F32 INI clamp bug fixed, grace auto-scale

- **Cursor tracking — round 2 of the Alt+Tab saga, much longer this time.** After the first
  Alt+Tab fix (recap below) merged, <user> found F1 input STILL died after Alt+Tab in a later
  test. Four full rounds of fix → user tests live → wrong or incomplete → next hypothesis,
  each grounded in something the user directly observed (not guessed):
  1. Fed every `GetCursorPos` read to ImGui unconditionally → cursor visibly snapped to/stuck at
     screen centre on open.
  2. Baseline-diff gate (don't feed until it differs from the first read) → user reported the
     centering was still happening — didn't fix it.
  3. Switched to tracking a virtual cursor accumulated from raw-input mouse deltas (captured in
     `hk_get_raw_input_data`/`hk_get_raw_input_buffer` right before blanking them for the game) —
     theorized `GetCursorPos` was simply frozen under Wine. Fixed the immediate symptom but user
     reported imprecision (drift growing with mouse travel) and an inconsistent seed/pivot point
     across launches (sometimes centre, sometimes near the top).
  4. Fixed the seed race (was falling back to a hardcoded 1920x1080 guess if `io.DisplaySize`
     wasn't populated yet) and the mickeys-to-pixels scale (via `SPI_GETMOUSESPEED`) — better, but
     user asked directly "why can't we just use GetCursorPos", which prompted actually re-reading
     `hk_get_cursor_pos`'s own body for the first time this arc. **Real root cause, hiding in
     plain sight:** that hook deliberately fakes screen-centre for ANY caller while `g_show` is
     true (to freeze the game's own 2D map-panning camera), with an existing exemption flag
     (`g_imgui_reading_cursor`) already used to let `ImGui_ImplWin32_NewFrame`'s own internal read
     through — nothing in the diagnostic/virtual-cursor code was ever setting that flag, so every
     "GetCursorPos is frozen" reading this whole arc was actually our own fake-centre trap. Final
     fix: set the exemption around the mouse-poll block's own `GetCursorPos` call, feed that real,
     exact value directly. **User-confirmed fixed.** The raw-input virtual cursor is kept only as
     a `[DIAG]` comparison value, no longer drives the real cursor.
  - Added along the way, all still in `config::debugCursorDiagnostic` (off by default): a live
    on-screen crosshair pair (cyan = raw poll, magenta = ImGui's `io.MousePos`) plus a text
    readout (positions, focus/capture state, `SetCursorPos` hook call rate + swallow state) — this
    is what let round 4 actually get resolved instead of guessing a 5th time. Worth keeping for
    any future input-tracking regression in this exact area.
- **Systemic F32 INI clamp bug, found via a completely unrelated report.** <user>: changing
  minimap zoom, saving, and restarting reset it to 5. Root cause: `IniType::F32`'s load-time
  clamp was a single hardcoded `[0.1, 5.0]` written for the overlay SCALE multipliers
  (`overlay_master_scale` etc.) but reused generically for every later F32 field. Audit found it
  silently also broke `minimap_size` (100 default → 5 on the very next save+load, effectively
  bricking the minimap), `icon_min_half_px` (8.0 → 5.0), `grace_offset_x/y` and
  `minimap_offset_x/y` (0.0, and any negative value, forced up to 0.1), and `minimap_opacity`
  (should be 0..1, not 0.1..5.0). Fixed with a real per-field `f32_min`/`f32_max` on `IniEntry`
  (default 0.1/5.0, so the fields that were fine stay unchanged) plus
  `ImGuiSliderFlags_AlwaysClamp` on the minimap sliders so a Ctrl+Click-typed value beyond what's
  shown can't silently diverge from what gets saved. **User-confirmed fixed in-game (2026-07-01):
  minimap settings now persist correctly across save+restart.**
- **Grace icon size mismatch — automatic fix, not a hand-picked constant.** <user>: undiscovered
  grace (`MENU_MAP_Player_02`, disk glyph) reads ~2x the discovered one (harvested live from the
  game's own rendered frame, `ensure_grace_srv`) in a screenshot. Traced the draw code: both share
  the exact same destination quad size (`gh`) — the difference is content-density, since
  discovered's raw screen-capture crop (156x156, confirmed via `[GRACE-SRV]` log) has real padding
  around the glyph while the hand-authored disk crop (42x62) is tight. User explicitly rejected a
  hand-picked per-icon multiplier and asked for the ratio to be automatic: `map_point_glyph_uv`
  gained optional native-pixel-dims out-params, `set_grace_sprite` gained the same for the
  discovered side, and the undiscovered branch now scales by
  `sqrt(itsNativeW*itsNativeH) / sqrt(discoveredNativeW*discoveredNativeH)` — derived from real
  measured rects each run (`[GRACEUNDISC]` log line prints both). **User-confirmed fixed in-game
  (2026-07-01): undiscovered/discovered grace icons now match size.**
- Branch `feat/minimap-scale-cluster-search` (12 commits, despite the name it grew well past the
  original minimap scope) merged to master this session.

## Session recap (2026-07-01) — Alt+Tab F1 input dead: root-caused after 3 rounds, fixed (FIRST round — see the recap above for the longer second round)

- Continuation of the gamepad-nav/kbd bug session below. After that session's fix (event-driven
  `g_has_focus`) was merged, <user> found Alt+Tab STILL killed F1 input in a fresh test. Rather
  than guess again, added `[KBDIAG]` logging (wm_char/keydown counts, WantCaptureKeyboard/
  WantTextInput/WantCaptureMouse, MousePos, the polled left-button state) and had the user repro.
- **Round 1 root cause (log-confirmed):** ImGui_ImplWin32 only refreshes `io.MousePos` via
  `WM_MOUSEMOVE`, which this game suppresses during normal gameplay (raw input) — same reason the
  left-button click is already polled via `GetAsyncKeyState` instead of read from
  `WM_LBUTTONDOWN`. A real Alt+Tab's `WM_KILLFOCUS` invalidates `io.MousePos` and nothing ever
  refreshes it again — log showed it pinned at ImGui's `-FLT_MAX` sentinel for 26+ seconds
  straight, so `WantCaptureMouse` stayed false even though the button poll correctly saw real
  clicks. Fix: poll `GetCursorPos`+`ScreenToClient` alongside the button, same pattern.
- **Round 2 regression (found by <user> in-game, not from a log):** that fix caused the cursor to
  visibly snap to / stay stuck at screen centre the instant F1 opened. Cause: this game keeps the
  OS cursor warped to centre continuously during normal play as part of its raw-input camera
  (same behavior `hk_set_cursor_pos`'s existing "swallow the game's recenter-to-middle" comment
  already described) — so the very first poll after opening genuinely reads back centre, and
  feeding that stale value into ImGui is what showed up as a stuck cursor. First attempted fix
  (baseline: don't feed a position until it differs from the first read) was judged insufficient
  by <user> — the centering persisted.
- **Final fix, user-confirmed working in-game:** stopped gating `g_show` (drives drawing AND
  every input-capture hook) on OS focus (`fg`) at all — it now depends only on `g_user_show`, the
  F1 toggle itself. This removes the focus TRANSITION entirely instead of continuing to patch
  each bug it produced (invalid MousePos, WantCaptureMouse never recovering, cursor pinned at
  centre) — a root fix for the whole bug class, not another edge-case patch. **Tradeoff (by
  design, accepted):** F1 now stays fully active, including input-swallow, even if the game
  window loses OS focus — if the user Alt+Tabs to interact with a DIFFERENT window while F1 is
  still open, our hooks can interfere with that window. Close F1 first in that case.
- Branch `diag/alt-tab-click-toggle` off master (despite the name, ended up being the real fix,
  not just diagnostics) — docs updated (`dx-bugs-backlog.md` item 3 followup, `changelog.md`),
  about to merge to master.
- **Lesson for next time:** the earlier "log-confirmed fixed" claim (gamepad-toggle debounce
  session, below) was premature — it fixed the FLAPPING signature it was diagnosing, but a
  different bug (MousePos never refreshing) was hiding behind it and only surfaced once the
  flapping stopped. A log confirming one hypothesis is not the same as the user confirming the
  original complaint is gone — get the live "yes it's fixed" before declaring done.
## Session recap (2026-07-01) — minimap honors marker-scale, gets its own clustering + search-ring (items 13/14)

- <user> picked items 13/14 as the next bug after this session's earlier fixes + doc audit.
  `draw_minimap` (`src/worldmap/map_renderer.cpp`) had a hardcoded `half=6.0f` (ignored
  `overlayMasterScale`/`overlayIconScale` entirely) and drew every marker individually with no
  grouping — a wall of icons at any real density, and no visual indicator for an active
  item-search "locate" target the way the worldmap has (a yellow ring around `search_hit()`
  markers).
- Investigated first: the minimap's projection is NOT the worldmap's pan/zoom `u,v` map-space
  system — it's a simple local, player-centred, north-up Euclidean projection
  (`dx=(worldX-px)*scale`, `dy=-(worldZ-pz)*scale`). So the worldmap's `draw_clusters` (coupled to
  hover/tooltips/distance-adaptive zoom) wasn't reusable/worth porting — wrote a lightweight
  screen-space bucket instead (round each marker's screen offset to a 14px cell, keyed by
  `(group, cellX, cellY)`; 1 member draws normally, 2+ draws a pile dot + count label).
- Fix: `half` now scales with the same two configs (clamped 3-10px so extreme settings can't break
  the small fixed-radius HUD); the same bucket loop draws the yellow search-hit ring
  (`search_hit()` is a `static inline` helper already in the same TU, no plumbing needed) around
  any cell containing a search-matched marker.
- Built clean (`build-linux`), deployed to `/home/iamacat/Games/ERRv2.2.9.6/dll/offline/`.
  Plan: `/home/iamacat/.claude/plans/federated-painting-aho.md` (session-local path, not in-repo).
  **NEXT: <user> to verify in-game** — scale slider visibly resizes minimap icons, dense marker
  clumps pile with a count, item-search "locate" shows the yellow ring on the minimap too. Commit
  + merge after confirmation, same loop as every fix this session.

## Session recap (2026-07-01) — dx-bugs backlog audited against git log, 5 items already fixed but docs never updated

- <user> asked to check which "still open" backlog items were actually already fixed in code.
  Cross-referenced `git log` against items 2/6/10/11/12/15: **2** (partial — cursor-recenter done
  via PR C `4ec2aa7`, key-hint auto-switch UI never built), **6** (cursor recenter on map reopen,
  same PR C commit), **10** (RequireFragment/Region heuristic, `fix(fragment-gate)` commits
  2026-06-27/29 — already in `docs/memory/bugs/README.md` as resolved but never crossed off the
  numbered backlog), **12** (mouse passthrough + cursor anchor, `b10e50e`+`2854600`, both
  **2026-06-18 — predate the 2026-06-28/29 bug report**, so <user> likely reported against a stale
  build), **15** (loot undercount/no ×N stacking, `62eb9a9`, reads all 8 `ItemLotParam` slots per
  the existing plan). **11** (double-draw) clarified as root-caused (double-DLL-load artifact per
  the "Known bugs" section below, not an open code bug) rather than flatly open.
  `docs/memory/bugs/dx-bugs-backlog.md` and `README.md` updated to reflect this. Real remaining
  open items after the audit: 4/5 (pause, needs an RE spike first), 13/14 (minimap — see recap
  above, now also fixed), 16 (native ER right-stick zoom, not investigated), F1 (native
  overworld→underground icon leak), F2 (locate pan clamped at fog-of-war boundary).

## Session recap (2026-07-01, Linux + <user> live-testing) — 4 post-PR-C-2 bug reports — DONE, log-confirmed + in-game verified

- <user> reported 4 issues after playing with `3bd9530`/`b12618f` (gamepad nav + virtual keyboard,
  both already merged to master): (1) wanted an ImGui flags/settings research doc; (2) mouse/ImGui
  dead after Alt+Tab away+back; (3) since the on-screen kbd landed, mouse couldn't scroll and
  keyboard couldn't type in the item/category search panels; (4) locate/pan on Item research felt
  stuck on the player's tile ("bug already tracked" per <user>).
- **This session is Linux (no local Windows game/controller)**, so instead of guessing blind the
  loop was: ship a fix → <user> builds+tests on Windows → report back → add targeted diagnostic
  logging (`[FOCUSDIAG]`/`[KBDIAG]`) when a guess turned out wrong → read the actual log → fix the
  now-evidenced root cause → re-verify. Two guesses were wrong before the log pinned the real
  causes; documenting the trail below since it's the more instructive part.
- **Bug 2 (Alt+Tab kills input) + a 2nd related bug (search bar loses keyboard with NO Alt+Tab) —
  3 distinct fixes, all on `goblin_overlay.cpp`:**
  1. *(Real fix, but not THIS bug's cause)* `g_last_input_was_gamepad` had no `fg` gate on its
     write and no debounce on the mouse→pad switch edge — a single frame of pad "active" (stick
     drift) could re-arm `recenter_cursor_to_window()` almost every frame, fighting mouse use.
     Fixed with a `fg`-gate + `kGamepadSwitchDebounceFrames` (5-frame) debounce. <user> re-tested:
     Alt+Tab still broken → not the actual cause of that bug, but a legitimate fix kept anyway.
  2. **Root cause of the Alt+Tab bug, found via `[FOCUSDIAG]` log:** a single genuine focus cycle
     produced **7 `g_show` rising-edges** in ~20s. `fg` was re-polled every present frame via
     `GetForegroundWindow()==g_hwnd`; under Wine that call transiently returns something else for
     a few frames during the Alt+Tab compositor transition, so the poll caught those and flapped
     `g_show`, closing+reopening the ImGui window and resetting all its focus state each time.
     Fixed: new `std::atomic<bool> g_has_focus`, set only by `WM_SETFOCUS`/`WM_KILLFOCUS`
     (event-driven, real transitions only), consumed by `fg` and the redundant `fgw` poll in the
     Proton click-workaround block.
  3. **A 2nd, distinct bug found via `[KBDIAG]` log** (<user>: "even without unfocus/refocus, the
     keyboard can lose the hook while searching"): same `g_show`-flapping signature but with **no**
     `WM_SETFOCUS`/`WM_KILLFOCUS` between the edges — `g_user_show` itself (the toggle) was
     flapping. Cause: the gamepad toggle-combo read (`combo_down && !g_prev_gamepad_toggle_down`)
     had zero debounce; a known XInput behavior (stale/glitchy read burst right after an app
     regains focus) could bounce it, each bounce closing/reopening the panel and losing the search
     `InputText`'s keyboard focus. Fixed: `kToggleGamepadDebounceFrames` (3-frame) debounce, armed
     once per press. Removed the now-dead `g_prev_gamepad_toggle_down`.
  - **Confirmed via log** (<user> repro'd again): a real Alt+Tab now produces exactly one
    `g_show` rising-edge matching the `WM_SETFOCUS`, with `wm_keydown` nonzero again within ~2s
    (was stuck at 0 for 15+s before); other rising-edges in the log (deliberate F1 opens/closes)
    are isolated, no more repeated bouncing. **<user> also confirmed visually: keyboard, mouse,
    and gamepad all work correctly now.**
- **Bug 4 — no code change, ruled out as a regression.** Read `take_locate_pos`/`loc_best` in
  `map_renderer.cpp`; already keys off the target marker's own projected coords, not the player's.
  Symptom matches the already-tracked **F2** (pan clamped at fog-of-war boundary) instead;
  cross-linked in the backlog doc, no new item opened.
- **Bug 1 — done.** Wrote `docs/re/imgui_config_flags_research.md`, a checklist of ImGui
  `IO.ConfigFlags`/settings tied to the gamepad/mouse/keyboard-coexistence bugs above, for future
  sessions to try one at a time instead of re-deriving ImGui's flag surface.
- Changelog entry added. **NEXT: commit on `fix/gamepad-input-flag-debounce`, then merge to
  master** (branch not yet merged as of this recap).

## Session recap (2026-07-01) — PR C-2 part 2: on-screen gamepad keyboard — DONE, in-game verified

- Follows directly from PR C-2 part 1 (below), same session, in parallel with a separate Windows RE
  agent working `feat/quest-npc-layer`.
- Implements the last gap from item 3 (`docs/plans/dx_bugs_backlog_plan.md` PR C-2 part 2, now
  DONE): typing into the 3 free-text fields (item search, category filter, quest NPC filter) via
  gamepad. A "Kbd" button opens a popup keyboard built from ordinary `ImGui::Button`s in a row grid
  — ImGui's own gamepad nav (already enabled in part 1) drives it for free, no custom D-pad cursor
  code. Layout choice (Alphabetical/QWERTY) is a new `virtual_keyboard_layout` config (`IniType::U8`
  reused, no new schema plumbing), persisted via the existing "Save to INI" button like every other
  plain setting.
- In-game verified (user, 2026-07-01): keyboard popup opens/types/persists correctly across both
  layouts on all 3 fields; mouse/keyboard text entry unaffected.
- Two bugs found + fixed during verification: (1) the "Kbd" button was invisible — placed via
  `SameLine()` right after an `InputTextWithHint` sized to 100% width, landing past the panel's
  right edge; moved to its own line. (2) **Mouse fully locked out after opening F1 via a gamepad
  combo** — a latent bug in part 1's cursor-recenter, only surfaced by this session's testing: the
  recenter's own `SetCursorPos` call generates a real `WM_MOUSEMOVE`, which `hk_wndproc`'s "clear
  the gamepad-input flag on real mouse activity" logic couldn't tell apart from a genuine user
  move — so it re-armed the "just switched to gamepad" edge on the very next frame if the
  controller was still reporting ANY activity (e.g. a hand resting on the stick), re-firing the
  recenter, generating another self-inflicted `WM_MOUSEMOVE`, forever — an infinite loop pinning
  the cursor at the window center every frame, making the mouse look completely dead. Fixed with a
  one-shot guard consumed by the very next `WM_MOUSEMOVE`, so only genuinely external moves clear
  the flag.
- Branch `feat/gamepad-virtual-keyboard` off master, committed — merging to master this session.
- **Next up, raised by user this session, NOT started**: dx-bugs-backlog followup **F2** — item-
  search "locate" pan is clamped to the visible/explored area, so a result inside the fog of war
  can't be recentered on (stays at the pan boundary). User's hypothesis: needs out-of-bounds (OOB)
  pan support. Already tracked with more detail in `docs/memory/bugs/dx-bugs-backlog.md` F2 —
  not investigated this session.

## Session recap (2026-07-01) — PR C-2 part 1: gamepad widget nav + input isolation — DONE, in-game verified

- Done in parallel with a separate Windows RE agent working `feat/quest-npc-layer`, off master.
  Follows directly from PR C (below) in the same session.
- Implements dx-bugs-backlog item 3's remaining gap (widget navigation, not just the F1 toggle) —
  `docs/plans/dx_bugs_backlog_plan.md` PR C-2 part 1, now marked DONE there. Turned out to be a
  one-line flag (`ImGuiConfigFlags_NavEnableGamepad`): the vendored ImGui Win32 backend already
  polls XInput and feeds `ImGuiKey_Gamepad*` every frame, no hand-rolled button mapping needed.
- The real work was **input isolation**: XInput is polled, not message-based, so simply enabling
  nav would let the game ALSO react to the same stick/buttons in the background (camera spin,
  character actions) while browsing the menu. Fixed by hooking `XInputGetState` itself (MinHook,
  same idiom as the existing `SetCursorPos`/`ClipCursor` hooks): a caller inside our own module
  (ImGui's nav, our own PR C poll) gets the real state; a caller outside it (the game) gets a
  connected-but-zeroed `Gamepad` struct while F1 is open. Caller identity via `_ReturnAddress()` +
  a new `goblin::self_module_range()` accessor (reuses `g_self_base`/`g_self_end`, already computed
  once in `goblin_crashdump.cpp` for crash triage — no second self-module lookup).
- First attempt returned `ERROR_DEVICE_NOT_CONNECTED` to swallow — WRONG, found in testing: that
  simulates an actual unplug, and games commonly back off / debounce reconnect-polling a
  "disconnected" slot, felt as the controller being unresponsive to the game for a bit after
  closing F1. Fixed to report SUCCESS with a real, advancing `dwPacketNumber` but a zeroed
  `Gamepad` struct — connected, just nothing held; any button released while F1 was open is
  delivered as a real release, so nothing can look "stuck held" once F1 closes.
- In-game verified (user, 2026-07-01): nav highlight, D-pad/stick move focus, A/B activate/cancel,
  character does NOT move/act while F1 open, normal controller behavior fully restored on close.
- Two more bugs found + fixed live during verification: (1) the combo recorder captured the very
  button used to click "Record gamepad combo" via nav (A) as the whole combo, before the user
  could press their intended buttons — fixed with a "wait for full release before listening" gate;
  (2) recording a SINGLE nav-reserved button (A/B/X/Y/D-pad) as the toggle meant every ordinary
  button click via nav ALSO closed F1 (not just the recorder — reported as breaking normal menu use
  entirely) — fixed by rejecting single-button combos where that button is nav-reserved. Also had
  to hand-fix one already-saved bad `overlay_toggle_gamepad = A` back to default in the ini
  (validation only guards NEW recordings, not what's already on disk).
- New followup opened during this session (not investigated yet): sometimes the worldmap zoom via
  the right stick stops responding — `docs/memory/bugs/dx-bugs-backlog.md` item 16. Unknown repro/
  cause; **first thing to check is whether it reproduces on a pre-PR-C-2 build** (i.e. whether our
  new `XInputGetState` hook is an unrelated pre-existing ER bug or a regression we introduced) —
  do NOT assume it needs a code fix from us before that's confirmed.
- Branch `feat/gamepad-nav-input-isolation` off master, committed — merging to master this session.

## Session recap (2026-07-01) — PR C: gamepad toggle + cursor recenter — DONE, in-game verified

- Done in parallel with a separate Windows RE agent working `feat/quest-npc-layer`, off master.
- Implements dx-bugs-backlog items 2/3/6 (`docs/plans/dx_bugs_backlog_plan.md`, PR C — now marked
  DONE there). New config `overlayToggleGamepad` (default `Y+R3`, `IniType::GamepadMask`, already-
  existing `parse_gamepad_combo` infra had no real consumer before this). XInput resolved
  dynamically (`xinput1_4`→`xinput1_3`→`xinput9_1_0`), polled per-frame in `hk_present` (no window
  messages exist for it). Cursor recenters via the already-hooked `o_set_cursor_pos` on two edges:
  mouse/kb→pad-only transition, and world-map (re)open.
- In-game verified (user, 2026-07-01): `Y+R3` toggle, pad-switch recenter, map-reopen recenter, and
  the new "Record gamepad combo" settings button all confirmed working end to end (log
  `[OVERLAY] Gamepad combo recorded: Y+LB+RB` matched the persisted
  `overlay_toggle_gamepad=Y+LB+RB` in the ini).
- Two bugs found + fixed live during verification: (1) recorder captured on the FIRST single button
  read instead of waiting for release, so a real multi-button combo never had time to form — fixed
  by accumulating the union of buttons held while armed and finalizing only on release; (2) the
  toggle-combo check ran even while recording, so pressing the CURRENT combo (to record its
  replacement) also flipped `g_user_show` and closed the very panel being recorded in — fixed by
  gating the toggle check on `!g_gamepad_combo_recording`.
- Branch `feat/gamepad-toggle-cursor-recenter` off master, committed, not pushed/merged — user's call.

## Session recap (2026-07-01) — F1 cursor lock after Alt+Tab: found + fixed + merged

- **RESOLVED + MERGED to master.** User found a 100% reliable repro for the long-standing "F1 panel
  cursor sometimes frozen" complaint: open F1 → Alt+Tab away → Alt+Tab back → cursor never responds
  again (no hover/click/move). Root cause + fix + full writeup: `docs/re/proton11_cursor_lock_re_prompt.md`
  (now marked RESOLVED). Short version: `hk_wndproc` only forwarded `WM_SETFOCUS`/`WM_KILLFOCUS` to
  ImGui while the panel was already visible (`g_show`, recomputed once/frame) — `WM_SETFOCUS` on
  refocus can arrive a frame early and fall through unforwarded, permanently desyncing ImGui's internal
  focus state. Fixed by forwarding those two messages unconditionally. Along the way, added
  `[CURSORDIAG]` call-counter logging on the 5 cursor/raw-input hooks (still in master, harmless/cheap)
  — this is what let the log evidence refute the doc's original H1/H2 hypotheses (Wayland/win32u bypass)
  before the real cause was found by reading the code.
- User-confirmed fixed in-game via the exact repro. Changelog entry added.
- Unrelated to this session's other branch (`feat/quest-npc-layer`, not touched/not merged) — that work
  is exactly where the prior session left it, see the recap below for its own status.
## RESUME HERE (2026-07-01d) — Quest NPC feature COMPLETE (merged to master)

- **State:** `feat/quest-npc-layer`, clean tree, build-clang + build-erte green (vanilla/convergence NOT
  built — pre-existing CONFIGURE failure on an incomplete bake, unrelated). Deployed + live-verified on
  ERR v2.2.9.6. Read `docs/memory/features/quest-browser.md` (the "ALL QUEST NPCs PINNED", NPC GLYPH,
  GATE DELETED, PIN PLACEMENT, SEARCH BADGE notes are this session's).
- **Landed this session (commits b672ae6, 977f785, fcf6544 + a pending cleanup commit):**
  1. Real NPC map glyph `MENU_MAP_80` instead of a circle (mod-agnostic disk path, `category_meta.cpp`).
  2. Pin ALL runtime quest NPCs (58 on ERR), not just the 3 hand-authored — `entity_world_pos(pinEntity)`
     lookup; 3 hand NPCs keep step-following, rest static; unnamed/asset-placed NPCs pin too.
  3. Deleted `quest_npc_quest_aware` (config/schema/getters/checkbox), `quest_npc_gated_out`, the layer
     `done==0` skip — feature is no longer legacy/unfinished.
  4. Merchant state fix: fallback pins show live `[concluded]`/`[in progress]` ONLY for hand-VETTED
     death-distinct fail_flags; unvetted runtime flags (Kalé etc.) show neutral `optional`.
  5. `[quest]` badge in the item search (goblin_overlay.cpp) for hits backed by a WorldQuestNPC pin.
  6. PIN PLACEMENT fix: prefer a base-overworld placement (was pinning Blaidd on his stray Nokstella
     underground copy → garbage pan). `[QUESTNPC-PIN]` diag (now gated behind `debug_logging`).
- **Before merge (mostly done):** cleanups committed (stale comment + unused `goblin_quest_gates.hpp`
  include removed; diag gated). **THE USER pushes/merges.**
- **Follow-ups (not blocking merge):**
  - `QUEST_GATES` generated data (`goblin_quest_gates.*`, 4 profiles) is now DEAD (no code consumer after
    the gate removal) — remove via the generate pipeline in a separate change (harmless additive data now).
  - 3 quest NPCs are `no-position` (extracted, no placement resolved → need their MSB source); 6 UG + 4
    DLC pins are plausibly-correct underground/DLC NPCs (Deeproot Fia/D, DLC followers) — spot-check if desired.
  - Boss-handler NPCs (`90005860`, e.g. Gurranq) NOT extracted (90005860 is EVERY boss's handler → would
    flood pins). Needs a quest-vs-plain-boss signal.
  - Tooltip quest/step prose is English (hand-authored) while the NPC name is localized.

## Session recap (2026-07-01b) — feat_quests Phase 2: per-step entity_id sourced + wired (offline)

## Session recap (2026-07-01b) — feat_quests Phase 2: per-step entity_id sourced + wired (offline)

- **Sourced + wired real per-step `entity_id`** for the bootstrap set in
  `src/generated/goblin_quest_steps.cpp` using `tools/_find_npc.py` (MSB placement lookup) joined to
  `data/tile_region_map.json` (BonfireWarpParam-authoritative tile→region) — deduction by region match,
  not guessing. Confident wires: **Alexander 1–5** (Stormhill / Gael Tunnel / Redmane / Mt. Gelmir /
  Farum Azula — 1/4/5 exact subRegion match), **Thops 1/2/3** (Church of Irith ×2 / Academy classroom),
  **Boc 1/5/6** (Limgrave bush / Altus ×2). Left `0` (no offline-disambiguable source): Boc 2 (Coastal
  Cave), Boc 3/4 (two ambiguous Liurnia placements), Thops 4 (corpse — needs EMEVD/in-game).
- **Resolved the two pre-existing candidate ids the prompt flagged:** `Boc 11050730` resolves to
  **Leyndell, Ashen Capital** = NOT any of Boc's 6 steps → correctly NOT used. `Thops 1039390700`
  resolves to **Liurnia (Church of Irith)** = step 1 → wired there.
- **Two blockers fixed en route (untracked-artifact / tooling):** (1) `tools/_find_npc.py` crashed on
  Windows (`frombytes` reused one temp filename while SoulsFormats keeps it memory-mapped) — now unique
  per call + best-effort unlink. (2) `tools/gen_nonerr_stubs.py` only wrote stubs *if missing*, so the
  verbatim-copied `.hpp` never tracked the schema migration and the synth `.cpp` never emitted the new
  `quest_step_done` free function → **every non-ERR build was broken since the helper was added**. Now
  refreshes hpp/cpp on content change and synthesizes no-op definitions for free functions.
- **Builds:** `build-clang` (canonical `generated`) and `build-erte` both green. `build-vanilla` /
  `build-convergence` still fail at CMake *configure* on missing `goblin_category_exceptions.hpp` etc. —
  **pre-existing incomplete data bake on this machine, unrelated to quests** (needs the full per-profile
  pipeline run; not attempted).
- **progress_flag TRACED — structural finding blocks naive wiring (still 0).** Chased it TalkESD →
  EMEVD offline (corpus IS present: `D:\tools\emevd_js\err`, 516 `.emevd.dcx.js`; my first-pass "no
  corpus" was wrong — I'd checked the DarkScript3 TOOL dir, not its output). Each NPC's quest flags are a
  **mutually-exclusive STATE REGISTER** (Boc ev3959 / Thops ev3819 / Alexander ev3679 in `common.emevd`):
  `BatchSet(lo,hi,OFF)` then `Set(value,ON)` — advancing CLEARS the prior, so one flag == done would tick
  then UNtick. Bonus: the register transitions are gated by AREA flags matching the entity_id regions,
  independently confirming the pins. **DECISION (user): extend the schema — DONE.** Added
  `QuestStep::progress_flag_max` (register hi); `quest_step_done` OR-scans
  `[progress_flag..progress_flag_max]` (= register ≥ value), else plain terminal check.
  `quest_npc_layer.cpp` active-step picker gained a `flag_floor` so a manual gap (Alexander's missable
  Gael Tunnel) or a concluded quest no longer traps the pin on an early step. WIRED (confident, anchored
  by transition location side-effects): Alexander steps 1/3/4/5 (`3666`/`3669`/`3670` register + `3663`
  death), Thops step 4 (`3803` concluded), Boc step 6 (`3943` concluded); the rest stay manual (no
  confident mapping / missable). Host-tested + build-clang & build-erte green. **Still NEXT:** in-game §7
  visual verify (game not running); filling the remaining mid-steps (Alexander step 2, Boc & Thops steps
  1-3) needs per-gate-flag RE or in-game capture. See `docs/memory/features/quest-browser.md`
  (PROGRESS_FLAG STRUCTURAL FINDING + DECISION + IMPLEMENTED).

## Session recap (2026-07-01) — feat_quests Phase 1: schema + entity-position cache + flag wiring + QuestNpcLayer

- **Implemented `docs/plans/feat_quests_implementation_plan.md` Phase 1 on `feat/quest-npc-layer`
  (forked from master, not merged).** Builds clean on `build-linux` (ERR profile); deployed to
  `dll/offline/MapForGoblins.dll`. **Log-checked after the user ran it** (`dll/offline/logs/
  MapForGoblins.log`, 2026-07-01 01:08-01:09): all AOB signatures PASS, zero errors/exceptions/AV,
  `[BENCH] build.quest_npc: 0.01 ms` fired exactly once (not every refresh cycle) confirming `QuestNpcLayer`
  is wired correctly and its cache/signature rebuild-skip works (no per-frame flag re-read). No regressions
  detected elsewhere in ~25s of normal overlay activity. Still NOT *visually* verified (no map pins exist
  yet to look at — see the unsourced-data note below) — this is a crash/wiring smoke-test, not a feature
  verification. Before coding, re-verified the plan's §0 infra claims against current master and found 2
  wrong (both corrected in the plan doc itself, not just here):
  1. The "legacy `WorldQuestNPC` emission ~L1891" the plan wanted to retire was already gone — only a dead
     skip-rule remained (`map_entry_layer.cpp`). Kept that skip-rule (didn't delete it, diverging from the
     plan's literal text) as a guard: `erte`/`convergence`'s local bake is stale and not yet regenerated
     (see `generated_data_removal_plan.md` Phase A), so deleting it now risks double-draw on those 2
     profiles until that separate plan's regen lands.
  2. "`prebuild_markers()` already builds a reusable entity→position index, REUSE don't duplicate" was
     false — verified `prebuild_markers()` is a thin trigger shim; the `ent_enemy`/`ent_any` maps that do
     exist are local to one disk-marker helper, not file-scope. Built a new `g_entity_pos` cache instead
     (populated inside the EXISTING disk-worker pass, still zero extra parsing) — exposed as
     `goblin::worldmap::entity_world_pos()`.
  3. **New bug found + fixed, not in the original plan:** the disk-worker's enemy/asset enumeration was
     gated behind unrelated loot toggles (`lootEnemyDrops`/`lootEmevdDrops`/`worldFeaturesFromDisk` etc) —
     would have silently broken quest pins for anyone with all of those off. Forced enumeration on
     whenever `show_quest_npc` is enabled.
- **What's real and working:** schema (`QuestStep::progress_flag/entity_id`, `NpcQuest::name_id/
  hostility_flag`), `entity_world_pos()` cache, `goblin::quest_step_done()` (shared by `qp_get`/`qp_set`
  AND `QuestNpcLayer` so they can't disagree), `config::questAllowFlagWrite` cheat gate (default off,
  read-only flag mirror with `[auto]` tag otherwise), hostility amber note, `QuestNpcLayer` itself
  (epoch-signature cache, sole `WorldQuestNPC` producer, excluded from the generic category loop like
  `GraceLayer`).
- **What's deliberately NOT done — the actual next task:** `progress_flag`/`entity_id` are 0 for every
  step of the bootstrap demo set (Boc/Alexander/Thops) → **QuestNpcLayer currently produces zero map
  pins.** No EMEVD/MSB cross-reference tooling was available offline (Linux, no decrypted regulation) to
  source real values safely. `name_id` WAS sourced for real (122310/122000/133300 from
  `data/npc_name_text_map.json`). Two candidate `entity_id` values exist in a pre-existing comment in
  `goblin_quest_steps.cpp` (Boc `11050730`, Thops `1039390700`) but weren't wired — unclear which of
  their multiple steps (they relocate across the map) the placement belongs to; wiring blind risks pinning
  the wrong location. Crash/wiring safety already confirmed (log above) — what's left is purely the
  DATA. **NEXT (Windows, EMEVD+MSB tooling):** `docs/re/windows_quest_npc_progress_flags_re_prompt.md` —
  source + verify real per-step `progress_flag`/`entity_id` for Boc/Alexander/Thops's 15 steps (uses the
  existing `tools/_find_npc.py` MSB lookup + the in-overlay `debugEventFlags` coverage-gap hook
  empirically, not blind reuse of the 2 unverified candidate ids or `quest_gates.py`'s wrong-semantics
  flags), then visually verify in-game (exactly one marker, correct position, `questAllowFlagWrite` OFF
  read-only behavior). Changelog entry deferred until this makes the feature actually user-visible (0 pins
  = nothing to announce yet).

## Session recap (2026-06-30 LATE) — native GetMessage: RE resolved → refactor landed → visually confirmed on ERR

- **Native message getter RE — RESOLVED + IMPLEMENTED + VISUALLY VERIFIED (ERR).** `CS::MsgRepositoryImp::GetMessage`
  = `FUN_14266d3c0` @ RVA `0x266d3c0`, `wchar_t* GetMessage(repo, group, fmgId, msgId)` (group=0, fmgId=physical
  slot, NULL on miss). It does NOT merge layers itself (repo `groupCount==1`); under ERR the loader folds DLC
  strings into the BASE slot and stubs the vanilla DLC slots, and **ERR hooks GetMessage** (live entry = MinHook
  `E9` trampoline). Full RE: `docs/re/windows_native_msg_getter_re_findings.md`. Verified read-only against the
  live process — no rebuild needed for the RE.
- **Refactor landed (`a64f4e1`).** `lookup_text` now resolves names on demand via GetMessage (per-id, cached),
  with `decode_textid()` mapping each marker band → layered slots `{dlc02,dlc01,base}` + real id. Eager FMG
  copies (`copy_fmg_entries`/`copy_fmg_all_layered`) NEUTRALIZED (no-op) so nothing hand-walks slots; sanitizer
  validity now = "lookup_text resolves a real string". Crash class structurally gone (GetMessage bounds-checks).
  In-game log (ERR, this session): `[SIG] PASS GETMESSAGE` (unique), `GetMessage resolved at 0x…d3c0` (= match−5,
  interior anchor correct), `setup_messages 11.26 ms`, `[SANITIZE] cleared 0`, no crash/`?PlaceName?`. **User
  confirmed labels render correctly in-game.**
- **AOB sig (`94fdff3`):** `GETMESSAGE` in `re_signatures.hpp` (interior-anchored, `entry = match-5`, since ERR's
  hook clobbers the prologue) + health-check entry.
- **Windows build UNBLOCKED (`fa75402`).** Two infra blockers fixed: `tools/lib` was missing `libzstd.dll`
  (ZstdNet regulation-decrypt) → added; SoulsFormats temp-`.bnd` unlink raced on Windows (PermissionError aborted
  generators) → `tools/sitecustomize.py` makes `os.unlink` retry-then-swallow. Run codegen with
  `PYTHONPATH=tools py tools/build_pipeline.py --profile erte`; then `cmake --build build-erte` (~13s incremental).
- **Cleanup commit — DONE (`c99b938`).** Deleted the dead id-collection loop, `copy_fmg_entries/_layered/
  _all_layered` lambdas + call sites, both `#ifdef MFG_VANILLA` slot-list/DLC-whitelist blocks, the stale
  EventTextForMap "unsupported" warning (decode_textid already covers the 600M/34/367/467 band — same
  GetMessage path as everything else), and the now-orphaned `seh_call`/`seh_run_job_thunk` helpers
  (`<functional>`/`<set>` includes too). `setup_messages()` ~625 → ~165 lines. Builds clean on the ERR
  (`build-linux`) profile, no unused-symbol warnings. `build-vanilla`/`build-erte`/`build-convergence` have
  no configured CMakeCache on this machine — recheck on Windows alongside item 2 below.
- **STILL OPEN / NEXT:**
  1. **vanilla+DLC verification** — only ERR is visually verified; the one-DLL-for-all claim (vanilla resolves DLC
     via the real DLC slots) is logically sound but untested. Build/deploy the vanilla profile and eyeball a DLC item.
     Also re-confirm EventTextForMap (600M / slot 34) actually resolves via GetMessage now that the dead
     per-profile machinery is gone (was unsupported before the refactor).
  2. **One-DLL map-data thread — plan written, not started: `docs/plans/generated_data_removal_plan.md`.**
     Correction vs the old framing: `goblin_map_data.cpp` (marker positions) is **already** an unconditional
     0-length stub in `tools/generate_data.py` for every profile — ERR and vanilla prove it on disk; erte/
     convergence's large local `generated_erte|convergence/goblin_map_data.cpp` (3.2 MB/3.0 MB) are just
     STALE pre-change artifacts (also missing newer generated files — confirms staleness, not "unmigrated").
     `generated_vanilla/erte/convergence/` are gitignored/untracked — nothing to delete from git. Real next
     step is Phase A of the plan: regenerate+rebuild+verify all 3 non-ERR profiles on Windows (closes the
     vanilla+DLC-verify item above in the same pass), THEN Phase B (dedup the now-identical stub +
     category_exceptions/name_aliases into `generated_shared/`), THEN Phase C (delete the dead
     `MAP_ENTRIES` consumer call sites for real, once C0 confirms 0 entries everywhere). Most of
     `generated_*` (enemy_names, region_anchors, quest_steps, ...) is MapForGoblins' own authored content,
     not a mod bake — **not** removable; full plan explains why.


## Session recap (2026-06-30 NIGHT) — spatial cull verified + loot NONAME closed + ViewDelay bug spawned

- **Loot "Unknown item" / NONAME followup #1 — RESOLVED (Aeonia = ERR-custom, not a bug).** Decoded the
  deployed-diag `[NONAME]` line `loc='Swamp of Aeonia'`: goods id **401**, lot **948380010** — BOTH absent
  from `data/items_database.json` ⇒ ERR-custom, so "Unknown item" is correct. Siblings 240/310/375 also
  absent; 9800 (Limgrave) present but `name=''` at source (nameless data, not a runtime preload bug).
  cat_bucket=16 NONAME lines (name_id 15xxxxxxx) = the known vanilla ammo FMG gap. So the live-fallback
  goods that show "Unknown" are genuinely ERR-custom, not a lookup regression. Followup #1 closed.
- **`feat/spatial-grid-cull` — VERIFIED + REBASED, ready to merge (not pushed; USER pushes).** Rebased
  clean onto master `838e388`, built+deployed `41199a6a`. In-game (run 2026-06-30 ~19:50):
  `render.worldmap.markers` **3.58 → 1.28 ms (~64%)**, clusters ~0.34ms. Proven visually invariant
  (margin == 256-unit pile cell ⇒ on-screen-centroid piles keep every member). Changelog + memory updated.
  Commit the doc updates on the branch, then it's mergeable.
- **FIXED + merged: zoom marker teleport.** Fix = `view_delay_zoom=true` (ON, the default): the basemap
  zoom is composited with the same ~1-frame lag as pan, so markers must delay ZOOM too (not just pan) to
  ride it — delaying pan alone left zoom mismatched and teleporting. `view_delay_frames=1.0` kept
  (user-confirmed). F1 slider + "Delay zoom too" checkbox remain. Merged to master (`31f29c0`, with the
  cull `8f7ef91`). Detail in `docs/memory/bugs/overlay-render-perf-followups.md`.

- **Loot ammo names — FIXED + MERGED (`aa373eb`).** Ammo (WeaponName id ≥50M) was preloaded UNSHIFTED but
  markers encode ammo at +100M → `lookup_text_utf8` missed → "Unknown item". The whole-namespace preload
  (`copy_fmg_all_layered`, `goblin_messages.cpp:659`) now emits BOTH keys (raw + 100M). Verified: `[NONAME]`
  dropped dozens → 5, all cat19 GOODS (310/9800/401/240/375 = ERR-custom suspects, placeholder correct),
  zero ammo left. Closes cause (a) of `docs/plans/loot_name_dx_followups.md` #1. Cause (b) (vanilla goods
  with names failing — Ember Piece etc.) did NOT reproduce on these pages — STILL OPEN if it shows elsewhere.

- **RE QUEUED — native message getter, to kill `#ifdef MFG_VANILLA` + the FMG slot-walk.** Wrote
  `docs/re/windows_native_msg_getter_re_prompt.md`. Context: loot-name resolution is already param-side &
  mod-agnostic up to the name FMG id (`resolve_loot_item_textid` `goblin_inject.cpp:4756` + `encode_live_item`
  `:1103`); the ONLY remaining per-mod dependency is name-id→string, done by hand-walking the MsgRepository
  slot array (`copy_fmg_layered` `goblin_messages.cpp:586`, `lookup_text` `:1079`). That hand-walk is why the
  `#ifdef MFG_VANILLA` (`:697`) pins ERR to base-only `{10}`: vanilla DLC slot numbers are ERR-wrong →
  v1.0.15 `?PlaceName?` = binder index/layout **corruption, NOT an AV** (seh_call can't catch it). Find the
  engine's own `GetMessage(category,msgId)` (merges base/dlc01/dlc02 internally, loader-correct) → call it
  instead → ifdef + slot table + ~100 lines deleted, DLC items resolve on every mod, corruption structurally
  impossible. Anchor: `MSG_REPOSITORY` sig `re_signatures.hpp:55`. Windows RE (Linux disk-verify Oodle-blocked).
  Independent of the runtime profile-detection chantier (the other half of the one-DLL goal).
- **Overlay menu unclickable on Wine/Proton — FIXED + MERGED (`9d6a261`).** F1 panel showed + hover worked
  but clicks didn't register. NOT cursor-lock/fullscreen (those theories were wrong, reverted): ER reads
  Raw Input, so newer wine/Proton posts NO legacy `WM_LBUTTONDOWN` → ImGui's message path saw no presses
  (position works because it's polled). Fix: poll buttons via `GetAsyncKeyState` and feed
  `io.AddMouseButtonEvent` each frame. Confirmed in log: `WM_LBUTTONDOWN seen while open: 0`. Branch
  `fix/proton11-cursor-win32u` (win32u/cursor diag, wrong theory) DELETED. Freeze guardrail recorded:
  `docs/memory/bugs/overlay-input-hook-freeze.md` (input detours run on the game thread — never block/loop).
  Proton note: 8.0 = bad fps (old VKD3D); 9/GE10/11 fine. Borderless still preferred for frame pacing.

## Plans live on master — fork implementation branches fresh (2026-06-30)
Policy: **plan-only branches are not kept.** A plan-only branch drifts as master's memory evolves (the
dx-bugs plan had already fallen behind the bug inventory). So plans live ON master under `docs/`, tracked
here; when implementation actually starts, fork a fresh branch from master. This avoids the data
divergence we cleaned up this session.

Plans currently on master, ready to start (fork from master when you do):
- **`docs/plans/feat_quests_implementation_plan.md`** (v2, audited) — quest browser automation + runtime
  `QuestNpcLayer`. Includes the salvaged NPC denylist appendix (from the retired `feat/quest-npc-layer`).
- **`docs/plans/dx_bugs_backlog_plan.md`** — DX bug/QoL backlog as PRs A–E, with a Reconciliation section vs
  the live inventory (`docs/memory/bugs/dx-bugs-backlog.md`, items 1–14 + F1/F2). Items 11/12/6 = the
  map-exit input softlock → see `docs/re/windows_input_softlock_re_prompt.md` (Windows RE, do that first).
- **`docs/plans/spatial_grid_opti_plan.md`** — clustering / spatial-grid optimization (PR E of dx-bugs depends
  on this).

Branches still open (NOT plan-only): `diag/fieldins-join-probe` (ours — 1 quentin452 commit `a6a5ce6`,
4-line MAPINS-walker diag in `goblin_collected.cpp`; left as-is). NOTE: `fix/marker-bugs` was NOT ours —
it was a local copy of `upstream/fix/marker-bugs` (VirusAlex's v2.0.x + yun-wulian Chinese localization,
0 quentin commits) on a divergent lineage; deleted 2026-06-30. Re-fetch only if cherry-picking an upstream
feature: `git fetch upstream fix/marker-bugs`.

## Session recap (2026-06-30 EVE) — loot naming + stacking + altitude + crash fix

LANDED ON MASTER this session (all built+deployed to ERRv2.2.9.6/dll/offline, several runtime-verified):
- **Item stacking → render-time** (`e1644c9`): was build-time (collapsed g_buckets, needed a rebuild on
  toggle). Now annotated once at build (non-destructive: rep + `stack_member` flags), toggle is a pure
  render decision → INSTANT, no rebuild. `annotate_item_stacks` + `is_active_stack` + Researcher counts
  +1/marker. Plan `docs/plans/item_stacking_plan.md`.
- **Crash fix — rebuild race** (`15c864e`): the old stack-toggle `rebuild_markers()` re-kicked a worker
  without waiting → two builds mutating g_buckets / a shared unordered_map → AV in rehash (`crash_320`,
  `+0x6B265`). Now serialized (single worker via `g_disk_running` CAS + `g_rebuild_pending`); worker is
  the only g_buckets mutator. `docs/memory/bugs/item-stack-toggle-rebuild-race.md`. (Render-time stacking
  also removed the toggle's rebuild path entirely.)
- **Off-page altitude via grace** (`ce2d8ce`): the ▲/▼ altitude badge now works on pages the player isn't
  on, referenced to the nearest grace in the marker's OWN area (player Y is out-of-frame there). Distinct
  tint (green/teal) vs warm/cool player-relative. `LiveGrace.posY` captured; `assign_grace_altitude_refs`
  at build. Plan `docs/plans/offpage_altitude_via_grace_plan.md` (DONE).
- **Cross-tile false-stack fix** (`f6faf6c`): stacking compared block-local raw_px/pz (0..256 per grid
  tile) → same-item markers in different tiles merged (Trina's Lily Fort Haight + Mistwood). Now full
  area-local coords (grid·256+pos).
- **Nameless loot placeholder** (`7616f21`, branch `fix/noname-loot-label`, NOT merged): `marker_label`
  returned only the location when the FMG name was empty → nameless AND dropped the `xN`. Now "Unknown
  item" + qty for loot/stacks. + `[NONAME]` build diag (behind `diag_loot_flags`, deduped by key,
  `c0c2a71`) listing resolved-key-but-empty-name markers.

OPEN INVESTIGATION — "Unknown item" name resolution (followup #1 in `docs/plans/loot_name_dx_followups.md`):
- Two distinct causes behind empty names: (a) **vanilla AMMO** (id 50000000 "Arrow" etc., cat2 +100M) —
  name exists in data but `lookup_text_utf8` returns empty at runtime = known ammo FMG gap
  (`docs/re/loot_ammo_encoding_finding.md`); (b) **live-fallback goods** (`[ITEMCLASS]` log, cat1 +500M) —
  MIXED: some genuinely ERR-custom (goods 240/401/2008015 absent from vanilla data), some VANILLA with
  names (Ember Piece 850010, Rock Heart 2002010, Haima Crown 1000000) whose runtime lookup STILL fails
  (preload gap). Goods names ARE whole-namespace preloaded (`copy_fmg_all_layered` GoodsName) when loot
  flags on, so a vanilla item showing "Unknown" = a real lookup bug, not ERR-custom.
- NEXT to pin "Swamp of Aeonia": user re-runs with `diag_loot_flags`, grabs the `[NONAME]` line with
  `loc='...Aeonia...'` → its key decodes (absent from vanilla data ⇒ ERR-custom; present ⇒ lookup bug).
  Deployed diag build md5 `c76044fa` (confirm the game loaded that md5 first).

RESOLVED — `feat/spatial-grid-cull` MERGED to master (`8f7ef91`), branch deleted post-merge:
Viewport-cull the marker hot loop landed and was verified in-game: `render.worldmap.markers`
**3.58 → 1.28 ms (~64%)** (see "Session recap (2026-06-30 NIGHT)" above for full detail). The
follow-on tile-based clustering work (a separate branch, `feat/spatial-grid`, implementing
`docs/plans/spatial_grid_opti_plan.md`) has also since merged (`c13caef`/`6df9866`), also deleted
post-merge. Plan doc's own status header is current; no open work tracked here.

INPUT SOFTLOCK — CAUSE FOUND (external): the map-exit "soft key lock" is actually triggered by the mouse
hitting a SCREEN EDGE and is caused by **Deskflow** (cursor-sharing KVM), not ER / not us. Fix is
Deskflow-side. See `docs/re/windows_input_softlock_re_prompt.md` (the F1 mouse-dead half may still be ours).

## Session recap (2026-06-30 PM) — loot count + stacking (superseded above; kept for detail)
- DONE `37f3239` (merge) loot item count + item stacking, verified in-game. (1) per-lot count in tooltip
  `xN`, weighted-roll aware (ItemLotParam slots = one weighted roll, not additive — sum was wrong);
  (2) item stacking: co-located identical-item markers within 5m merge → one `xN` (build-time, world-pos,
  works underground; toggle `stack_identical_items`); (3) depletion: `xN` counts down as gathered,
  stack grays only when all collected; (4) item Researcher counts instances (invariant to stacking).
  Plans: `docs/plans/loot_item_count_plan.md` (DONE), `docs/plans/item_stacking_plan.md` (v2 DONE).
- DONE `07b3904` bonus-2: SummoningPools → MENU_MAP_89 (Martyr Effigy), verified in-game (246 pools, live param).
- DONE `caed7ef` per-item loot icons: lot → real EquipParam iconId, native_item_icon → rep → atlas → circle. Verified working.
- DONE `8c16b60` bench lag-spike WARN (relative-to-baseline, even for quiet timers) + spikes column.
- DONE `d792a3a` instrument `draw_minimap_hud` as `render.minimap`. RESULT (run 13:10): spikes only
  ~3ms (`~600x` a 0.01ms avg) — **minimap EXONERATED**, not the felt map-close lag.

## Baked-atlas removal (#4) — DEFERRED to a future PR (2026-06-30)
Decision: do NOT remove the baked atlas on `feat/native-poi-icons`. Audit proved it's still load-bearing
(~15 categories). The `[ICONTIER]` census tool stays in the tree for re-auditing. Revisit in a dedicated
PR after native coverage widens (see follow-ups below). Audit detail:
Gate before deleting the baked overlay atlas: prove which categories actually need it per mod.
- DONE `92d300c` `[ICONTIER]` census in map_renderer.cpp: tags each IconHandle's resolve tier
  (mp_name / mp_id / item / rep / atlas / circle), tallies per draw pass, logs a throttled summary
  + the category names that hit the baked atlas or a circle. Audit-only, no behavior change.
- FIXED `internals/modengine/vanilla.me3` (game dir, not repo): was loading the forbidden stale
  `MapForGoblins_vanilla.dll` → now `MapForGoblins.dll` (single-DLL rule). Both ERR + vanilla use the
  one deployed DLL.
- Vanilla = clean Steam copy: `/home/iamacat/.local/share/Steam/steamapps/common/ELDEN RING/Game/eldenring.exe`
  (ERR injects via ModEngine, does not touch the Steam base files).
- RUN: ERR (normal launch) → open map ~5s → grep `[ICONTIER]`. Then vanilla:
  `cd internals/modengine && ./bin/me3 launch -g eldenring -e "<steam eldenring.exe>" -p vanilla.me3`
  → open map ~5s → grep `[ICONTIER]`. DIFF the two: native/rep in ERR but atlas/circle in vanilla =
  what removing the baked atlas regresses.
- RAN both (2026-06-30 13:29 vanilla / 13:32 ERR). Census is PER-VIEW (only on-screen markers), so the
  two runs sampled different regions → not a clean per-category diff; use the UNION.
  - Vanilla steady: `mp_name=0 mp_id=540 item=13282 rep=397 atlas=1064 circle=0`. Atlas-dependent:
    Hostile NPC, Spirit Springs, Spiritspring Hawks, Stakes of Marika, Kindling Spirits, Interactables.
  - ERR: `mp_name=4 mp_id=1 item=2099 rep=539 atlas=100 circle=7`. Atlas-dependent: Cookbooks,
    Crystal Tears, Consumables, Scadutree Fragments, Pots-n-Perfumes, Bell-Bearings, Crafting Materials,
    Golden Runes (Low), Rune Arcs, Stakes of Marika. circle: **World - Maps** (no glyph anywhere).
  - VERDICT: **gate NOT passed** — ~15 categories across both runs still depend on the baked atlas;
    removing it now regresses them to circle. KEEP the atlas.
  - Follow-ups before re-auditing: (1) loot cats hitting atlas instead of the per-item/rep tier
    (Bell-Bearings, Crafting Materials, Rune Arcs…) — per-item coverage gap? possibly free wins.
    INVESTIGATED (2026-06-30): all 9 are lot-backed treasures (push_marker lotType=1,
    map_entry_layer.cpp:378), so the per-item resolution DOES run for them but yields item_icon_id=0.
    Miss is one of two — `resolve_loot_item_textid` returns a baked textid <100M (lot not resolving to
    a live item) OR it resolves but `item_real_icon_id` returns -1 (the goods row's EquipParam.iconId
    is 0). Rep also misses (category_meta has no static rep for these). DISAMBIGUATOR = the tooltip:
    real item name → key resolves → iconId=0 (often fundamental); generic/baked label → lot-resolution
    gap (fixable, lotType/empty-lot). RESUME PROBE: add a one-shot debug log in push_marker for
    lot_backed markers where item_icon_id==0, dumping lotId/lotType + the resolved key + item_real_icon_id
    result, run ERR over an affected region, read the truth. Not started — belongs with the atlas PR.
    (2) wire numeric `category_gpu_iconId` for the world-feature cats (like summoning-pools→89).
    (3) `World - Maps` has no native glyph. Re-audit on a MATCHED map view once coverage widens.

## OPEN — deferred for later (2026-06-30)
0. **Loot item count (undercount + ×N stacking).** Root-caused: lot readers fetch only `ItemLotParam`
   slot 01, dropping slots 02-08 + `lotItemNum` quantities → multi-item lots show as 1 (e.g. "Below The
   Well" 1 vs 3 Sliver of Meat). Fix = read all 8 slots + quantities → `Marker.count` → "×N" badge. Full
   plan: `docs/plans/loot_item_count_plan.md`. Bounded multi-file wiring; deferred for a fresh context window.
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

## `feat/native-poi-icons` — RESOLVED, MERGED to master, branch deleted
Fast-forward merged (`887890b`) same arc this section used to describe as active; branch no longer
exists locally/remote (deleted post-merge per the merged-branch-hygiene rule). All bonus work landed and
is committed: bonus-1 undiscovered-grace icon (`6e45986`), bonus-2 SummoningPools glyph (`07b3904`),
per-item loot icons (`caed7ef`), `[ICONTIER]` census (`92d300c`). Baked-atlas removal was evaluated and
explicitly DEFERRED (see "Baked-atlas removal (#4)" section above) — gate not passed, ~15 categories
still depend on it. Bonus-3 (quest NPC → `MENU_MAP_80`) was deferred to the quest-NPC-layer work, see
"Parallel workstreams" below.

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

## QUEUE — all items resolved (see `feat/native-poi-icons` section above)
Bonus-2, per-item icons, and the baked-atlas decision are all DONE/committed/merged (detail above + in
the "Baked-atlas removal (#4)" section). Only real remaining item: **Bonus-3 quest NPC → `MENU_MAP_80`**,
deferred to the quest-browser work — see "Parallel workstreams" below.

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

## Parallel workstreams (no branch yet — fork fresh from master when starting, see policy above)
`feature/dx-bugs-backlog` and `feature/spatial-grid-opti` branches no longer exist (the spatial-grid work
already shipped — see `feat/spatial-grid-cull` resolution above; dx-bugs was never implemented and its
plan now lives on master per the "Plans live on master" policy). Pick up via the plan docs, forking a
fresh branch when implementation actually starts:
- DX bugs backlog → `docs/plans/dx_bugs_backlog_plan.md`, `docs/memory/bugs/dx-bugs-backlog.md`
- Quest browser + NPC layer (bonus-3 `MENU_MAP_80` lands here) → `docs/plans/feat_quests_implementation_plan.md`,
  `docs/memory/features/quest-browser.md`
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
