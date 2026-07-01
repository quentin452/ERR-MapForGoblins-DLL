# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.
Last updated: 2026-07-01 (`feat/quest-npc-layer` MERGED to master — quest NPC feature complete, its
RESUME HERE is below the Alt+Tab recap. Earlier same day: Alt+Tab F1-input bug fixed + merged (see recap
below; 3 rounds of fix-log-fix). `feat/minimap-scale-cluster-search` still open, not yet verified in-game).

## Session recap (2026-07-01) — Alt+Tab F1 input dead: root-caused after 3 rounds, fixed

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
