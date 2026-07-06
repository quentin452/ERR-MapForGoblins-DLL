# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping (2026-07-03, done):** file had grown to 1254 lines, mostly narrative for work already
merged, changelog'd, and in-game verified. Compacted to genuinely live/in-progress work, open
questions, and standing knowledge (gotchas, deferred decisions, non-obvious facts) not captured
elsewhere. History for anything not below: `docs/changelog.md` first, then `docs/plans/*.md`,
then `docs/re/*.md` (RE findings) and `docs/memory/`.

## ⇒ SESSION WRAP 2026-07-06f (Linux/Opus) — ★ off-VM converter affine VALIDATED + SHIPPED (fd0ad45 done): projection works map-closed, prime dropped

Implements + validates the "Remaining" step of the resident-converter-affine RE (`fd0ad45` /
`docs/re/windows_worldmap_affine_resident_source_re_findings.md`). `test_converter_offvm.py` **10/10** live.

- **✅ VALIDATED:** off-VM `proj_conv 60 42 36` reproduces `u=3712 v=7296` map-closed; capture-replay
  du/dv==0 vs the live VM. The origin caveat is settled the STATIC way — live `conv_affine 60` =
  `gxbase=28 gzbase=64 origin=0` (the 7168/16384 offset is in the grid decode, not the origin).
- **✅ SHIPPED:** `worldmap_probe::project()` + `get_converter_affine()` now fall back to an off-VM
  base-affine build (areas 60/61/12; `origin 0 / gridbase 28,64 / bias 128 / scale 1`, `legacyNode 0`)
  when no live VM → `proj 60 42 36 → u=3712 v=7296 page=0` **with the native map NEVER opened**. Confirmed
  live. Legacy-dungeon folds still need the VM node ptr → those return false off-VM and the caller keeps
  `legacy_fold`/baked (no regression). The "silent prime"/`world_map_open()` coupling is gone for the base
  overworld/DLC/UG projection.
- **Primitives:** `project_offvm()` (build 0x30 slot in our memory, run resolved `FUN_140876140`), RPC
  verbs `proj_conv` + `conv_affine`, `test_converter_offvm.py` (registered in tasks.json). No engine
  builder needed → `WORLDMAP_VM_CTOR`/`CONV_BUILDER` AOBs skipped. Both builds green + deployed.
- **✅ (2) DONE — audited the map-open gates.** No committed "silent prime" existed to remove. The
  remaining `world_map_open()`/`map_open` gates are all LEGIT (heightfield probe, D3D12 scissor/viewport
  tagging, mouse cursor, native-map search-locate) — none forced a prime. The vmap tile-fit paths
  (`panel_virtual_map.cpp` 456–605) still need the map open, but for **tile RESIDENCY** (`harvest_resident_tiles`),
  not projection — so not simplifiable by this change. Fixed the genuinely-stale bits: the `project()` /
  `get_converter_affine()` hpp doc comments (were "Map must be OPEN"; now document the map-closed base
  fallback), and reconciled `docs/plans/procedural_map_derivation_design.md`'s "never hardcode" rule with
  the now-proven exe-invariant fallback (live still preferred; hardcoded only for the proven-invariant base).
- **✅ OFF-VM COVERAGE EXTENDED to ALL placed areas (2026-07-06).** `project()` → `project_no_vm`: base
  affine for overworld (60→page0) + DLC-OW (61→page10) direct; legacy dungeons + base-UG (12) + DLC-UG fold
  via the resident `WorldMapLegacyConvParam` (`goblin::legacy_fold`, no VM node ptr) then base-affine, page
  from the folded area. Gotcha found live: base-UG 12 has overworld fields but its slot carries a conv node,
  so a raw area-12 point must be FOLDED (direct 12 mis-projects) — 12 routes through the fold path, not the
  direct shortcut. Validated `test_converter_offvm.py` **11/11**: live `proj` == forced off-VM `proj_nvm`
  (same u,v,page) over base + legacy m10/m11/m30/m35 + DLC-UG m40 + base-UG m12 (8/8 placed, all agree). New
  RPC `proj_nvm` forces the no-VM path so the test can compare with the map open (VM cache would shadow it).
- **✅ DLC-UNDERGROUND (areas 40–43) PROJECTION DONE (2026-07-06f2).** `project_dungeon_row_to_overworld`
  used to return false for 40–43 UNCONDITIONALLY (the dead "DLC eyeball path") → DLC-UG markers drawn by
  WORLD coords stayed native (off the DLC page). Now gated on `!conv_underground` like area 12, so the
  overlay path folds 40–43 → area 61 via the resident `WorldMapLegacyConvParam` (`legacy_fold`, same as the
  validated `project()` path). Verified live: `vmap dump_markers` → 349 area-40–43 markers, all group 2 (DLC),
  world coords `X[11201..13312] Z[10170..12217]` INSIDE the DLC-OW (area 61) cluster (was native ~0–1000, off
  page); none in the 19 `vmap offmap` margins. Group stays plain DLC (marker_group_from keys off original area).
- **vmap gamepad M4 — reticle → hover/warp/place LANDED (2026-07-06f2, needs PAD live-test).** The
  right-stick reticle (`s_pad_cursor`) is now the effective canvas pointer in pad-mode: it drives the
  single-marker/grace **hover tooltip** (accumulator uses `vptr`/`hovered_eff`), **FaceUp (Y/△)** warps a
  hovered discovered grace, **FaceLeft (X/□)** places/deletes a custom marker at the reticle. Buttons chosen
  to NOT collide with ImGui nav (FaceDown=Activate, FaceRight=Cancel, dpad/LStick=widget nav). **Mouse path
  byte-identical** when not in pad-mode (`vptr=io.MousePos`, `hovered_eff=hovered`, pad buttons false) —
  smoke-verified no regression (vmap renders, mouse right-click place + `vmap group` read OK). The OSK (item
  #1) was already done (`draw_gamepad_keyboard_button` = a real A–Z popup + Space/Bksp/Clear/Done, nav'able).
  **⚠ CANNOT live-test the pad from Linux RPC (no XInput injection) → USER must pad-test.** **STILL OPEN:**
  (a) pile/fan CLUSTER pad-hover (piles/spiderfy still read `io.MousePos` — reticle only hovers SINGLE
  markers so far; matters zoomed-out); (b) live-tune stick pan/zoom speeds + signs.
- **✅ PLAYER-DIMENSION AUTO-FOLLOW VERIFIED LIVE (2026-07-06f2).** The vmap auto-switches PAGE to the
  player's dimension on a crossing (`s_follow_player_dim`, edge-detect on `get_player_map_pos` group). Drove
  it live via grace warps with the vmap open: OW(0) → Nokstella base-UG → page auto-followed to group 1
  (`[VMAP] follow: crossed to group 1`), → Belurat DLC → group 2, → back to Nokstella → group 1. Player dot
  lands on the unified coords each time (Nokstella (9450,12052)==grace, Belurat (11266,11107)==grace) —
  confirms today's DLC-UG fix for the player dot too. Group derivation reuses the validated
  `project_dungeon_row_to_overworld`+`marker_group_from`. Added `vmap graces [group]` (warp-target dump) +
  `vmap group` NO-ARG read (report current page) for the test. Open-edge focus-on-open is robust
  (`s_was_open` tracked before the early return). No bug found — feature is correct + verified.
- **★ NEXT (follow-up, not blocking):** the vmap now has full map-closed projection — could drop any
  remaining "open the native map once" UX for projection (tile RESIDENCY still needs it; projection doesn't).

## ⇒ SESSION WRAP 2026-07-06f-pre (Linux/Opus) — off-VM converter affine: validation HARNESS built (fd0ad45), empirical run env-blocked

Implements the "Remaining" step of the resident-converter-affine RE (`fd0ad45` /
`docs/re/windows_worldmap_affine_resident_source_re_findings.md`): prove the world→map-space affine is
reproducible with the native map NEVER opened, so the VM/"silent prime" coupling can be dropped.

- **Off-VM projection wired, no engine-builder needed.** `worldmap_probe::project_offvm(...)` builds a
  0x30 converter slot in OUR memory from caller fields (`legacyNode=0`, base affine only — legacy fold
  stays with `legacy_fold`) and runs the resolved `FUN_140876140` map-closed. So the
  `WORLDMAP_VM_CTOR`/`WORLDMAP_CONV_BUILDER` AOBs the findings mentioned were NOT required (skipped).
- **RPC verbs** `proj_conv` (off-VM proj) + `conv_affine` (capture live slot fields) → `rpc-commands.md`.
- **Test** `tools/rpc_tests/test_converter_offvm.py` (registered in `.vscode/tasks.json`): (1) never-opened,
  projects `60/42/36` under BOTH candidate constructions — `unified` (origin 7168/16384, gridbase 0) vs
  `static` (origin 0, gridbase 28/64) — to settle the findings' origin-0-vs-7168 caveat by which hits
  `u=3712 v=7296`; (2) capture-replay open→capture→close→off-VM, assert `du/dv==0`.
- **Both builds green + deployed.** 
- **★ NEXT (env-blocked this session):** the box had a pre-existing **D-state `eldenring.exe` husk** (RSS 0,
  unreapable GPU/IO wedge — the documented freeze) so a cold-boot was unsafe → the empirical run is PENDING.
  On a clean box: `python tools/rpc_tests/test_converter_offvm.py`; the `[OFFVM]` line names the winning
  construction, `du/dv==0` confirms droppability. **Only after a green run** wire `project()`/
  `get_converter_affine()` to the off-VM fallback + drop the prime (do NOT ship the fallback unverified).

## ⇒ SESSION WRAP 2026-07-06e (Linux/Opus) — ★ native enemy names IMPLEMENTED (data path shipped, ImGui path deleted) + all-DejaVu overlay font

- **✅ NATIVE ENEMY NAMES — IMPLEMENTED (the 07-06d plan below is DONE).** `src/goblin_enemy_names.cpp`
  `update_native_enemy_names()` (host present-thread, called from `goblin_overlay.cpp` gated on
  `config::enemyNames`): per frame walks the visible `entityHpBars`; for each `nameId==0` TYPE our resolver
  can name (tiers 2/3), it injects a `NpcName` string (slot 18, reserved id band `810000000..899000000`,
  one seq id per npcParamId, batched into ONE FMG rebuild per frame) then writes `NpcParam.nameId` (+0x0c
  s32) once per type → the engine renders our name in its own native red tag. Idempotent per npcParamId
  (`s_assigned`). **ImGui path DELETED**: `draw_enemy_bar_names`, the `get_enemy_bar_labels`/`EnemyBarLabel`
  render feed + its overlay-api export, the `enemyNameScale`/`enemyNameOffsetY` cfg + F1 sliders. Kept
  `enemy_display_name` (map boss-marker supplement) + the `enemyNames` on/off toggle. Both builds (single +
  hot-reload split) link green; deployed. **✅ IN-WORLD VERIFIED 2026-07-06f2** (Linux/Proton): an aggroed
  generic **Godrick Soldier** (vanilla nameId=0 → normally blank) rendered "Godrick Soldier" in the engine's
  NATIVE red tag, with `[ENEMYBAR] reconcile: +1 named, -0 reverted, 1 (re)injected` in the log. Single tag
  (no dup), correct name, no visible gameplay side-effect (soldier aggroed/attacked normally), `810000000`
  band resolved cleanly (no garbage → free on this install). Screenshot en3_fight. Residual (low): accents on
  a FR install + the invader/summon/aggro-text side-effect aren't stress-tested, but the FMG path is UTF-8
  and nameId read as display-only in the earlier RE — treat as done unless a report surfaces.
- **Overlay font → single embedded DejaVu Sans** (dropped the ProggyClean+TTF merge + the GlyphOffset fudge;
  15px, live-tuning knob). Needs game restart (host-side atlas). Commit `4fc548c`.

## ⇒ SESSION WRAP 2026-07-06d (Windows/Opus) — ★ native enemy names SOLVED (engine data-path, proven live) + minimap-coupling bug fixed; LINUX takes implementation

Windows box, live RE on the running game. Two landed things + a major unblock handed to Linux.

- **★★ NATIVE ENEMY NAMES — mechanism SOLVED, native path PROVEN LIVE.** The red enemy name is drawn by the
  **VANILLA ENGINE** (find-what-writes on Varré's `EnemyTag_ColorText` → writer RIPs in `eldenring.exe`;
  `reforged.dll` has no Scaleform/name strings), from **`NpcParam.nameId → NpcName`**, and the engine
  **re-reads `nameId` LIVE**. FFDEC of the gfx: `EnemyTag_ColorText` is a trivial MovieClip, **no gating**
  → GATE Q1 = YES. **Proven end-to-end on a GENERIC** (Aigle nameId=0): `fmg_set 18 999001 "TESTAIGLE_MFG"`
  + `param_set NpcParam 60010010 0xc s32 999001` → native red tag rendered our string (screenshot). So the
  Scaleform-RE plan is **superseded**: the solution is a pure DATA path (inject NpcName + set nameId for
  nameId==0 generics, using our existing resolver), then delete the ImGui draw → kills the overlap + jitter.
  Offsets: **`NpcParam.nameId` @ +0x0c s32**, **NpcName FMG slot 18**. Full impl sketch + risks in
  `docs/plans/native_enemy_names_scaleform_plan.md` "★ THE SOLUTION"; RE in
  `docs/re/windows_enemy_name_hud_feed_re_findings.md` (SOLVED banner). **→ LINUX IMPLEMENTS** (the
  daily-build box): wire the per-type inject-and-set in the enemy-bar loop, pick a collision-free NpcName id
  band, verify no gameplay side-effect of setting nameId, delete `draw_enemy_bar_names`, keep
  `enemy_display_name`.
- **✅ Minimap-coupling bug FIXED (`dab061c`).** Enemy-name draw was gated behind `if (minimap)` (called
  inside `draw_minimap_hud`) → invisible whenever `show_minimap=false` (the Windows ini; Linux has it on) →
  looked "Linux works / Windows doesn't". Now `if (minimap || config::enemyNames)`; the minimap self-gates
  internally. Host-side → restart to load. Post-mortem in `docs/memory/bugs/README.md`.
- **Tooling proven on Windows:** external RPM + capstone live-disasm (found the timestep dt driver + the
  enemy-name writer), FFDEC gfx decompile, `fmg_set`/`param_set` for the data-path test. `mem_fwa off` (from
  70d8ff3) unblocked the capture. Windows build+deploy: `build.bat` → `build-err/` → copy to `dll/offline/`.

## ⇒ SESSION WRAP 2026-07-06c (Linux/Opus) — DX/UX sweep, freeze detection, live enemy-name capture (routed to Windows), font + gamepad + OSK

Long session on top of 06/06b. Local master well ahead of origin; USER pushes. All committed.

**Shipped (all render-side/hot-reloadable unless noted):**
- **Font accents** — `d23b779` fixed é/è rendering blank (ProggyClean shadowed DejaVu at 0xA0-0xFF → restricted
  ProggyClean to ASCII). `7971908` aligned the merged DejaVu baseline (was ~1px RAISED: cfg.GlyphOffset.y=1.0)
  + OversampleH=2. **★ QUEUED: DejaVu-ONLY font refactor** (drop ProggyClean, one TTF for everything → kills the
  bitmap-vs-TTF raised/blurry mismatch) — scoping subagent writing `docs/plans/dejavu_only_font_refactor_plan.md`.
- **vmap/minimap player bugs** — `be57cdd` player cursor now draws AFTER region labels (was covered); minimap
  halo fades with zoom like the vmap. `75b2b99` re-curved the minimap halo (was too weak: gone by default zoom
  2.0 now, full only when zoomed OUT). Host font init needs restart; the rest hot-reloads.
- **M1 heightfield (Track A)** — `52c9fa4` cast-window widen ±2000→+3000/-10000 + sea-tag plumbing (dormant
  `kSeaLevelY` sentinel until a live shoreline `hf_probe` calibrates it). Gamepad nav already wired (verify-only).
- **vmap gamepad canvas control (M4)** — `4933ea2` LEFT stick pan, L2/R2 zoom about a RIGHT-stick virtual-cursor
  reticle; latch exits on mouse move. **`7805e60` on-screen Kbd button** on the vmap grace + item search — REUSED
  the existing `panel::draw_gamepad_keyboard_button` (no new OSK logic; user caught the dup). Live-tuning + wiring
  the reticle into hover/click/place still open.
- **★ FREEZE DETECTION (host, `be57cdd`)** — `status` now carries `frame=<N>` (present-thread heartbeat): poll
  twice, unchanged-while-alive = the render/main thread is frozen (RPC listener is a separate thread → `ping`
  lies). The existing `freeze_watchdog` (default 20s) already dumps on a present stall; this makes it RPC-visible.
- **`exit` RPC verb (host, `432b112`)** — `exit`/`quit`/`kill_game` → TerminateProcess(self). Kills a SOFT freeze
  (listener alive, main thread futex-stuck); does NOT reap a D-state kernel wedge (same rule as SIGKILL).
- **FWA disarm (`70d8ff3`)** — `mem_fwa off` + WRITE/READ log label (unblocked the enemy-name capture).

**Enemy-name write-site capture — live progress, then ROUTED TO WINDOWS:**
- Confirmed live (headless, `39bb073`): the movie loads from the active install; the name is
  `<FONT LETTERSPACING='0'>Tree Sentinel</FONT>` (SetTextHTML). Full loop proven (boot, engage the Gatefront
  Tree Sentinel with `key w`, `pause` to survive, scoped scan, `mem_fwa … w`). NOT captured: the write is
  one-shot on bar-show + the buffer is freed on death/reload → needs a no-reload de-aggro.
- **★ D-STATE FREEZE INCIDENT + lesson:** a full-address-space `/proc/mem` scan (`scan.py`) swap-thrashed the
  Proton game into UNKILLABLE D-state (SIGKILL/Alt-F4/internal exit all pended; cleared only when the GPU/IO
  aborted). **RULE (`d7431e9`): scan-heavy RE → the WINDOWS box** (native RPM). On Linux, scan rw-p heap ONLY,
  capped (`scan2.py`, survived twice); in-DLL probes are always safe. The enemy-name write-site capture is now
  the Windows agent's job; Linux's part (buffer shape + tooling) is done.

**RE plans written:** `game_timestep_freeze_re_prompt.md` (fix the pause resume-latency by zeroing the
`FUN_140623410` dt — Windows agent already answered dt=xmm1 arg, no scale global; path B = hook & scale).

### ⇒ NEXT SESSION
- **★ WAITING ON THE WINDOWS AGENT: GFX enemy-name write-site RE** (`windows_enemy_name_hud_feed_re_findings.md`
  finish recipe) + the mod-agnostic GATE Q1. Don't delete the ImGui enemy-name path until Q1 = "vanilla shows
  the tag when fed".
- Review + maybe implement the **DejaVu-only font plan** (subagent-scoped this session).
- Track A continues: M3 cover-opaque vmap; wire the gamepad reticle into hover/click/place; live-tune stick speeds.

## ⇒ SESSION WRAP 2026-07-06b (Windows/Opus) — native-name plan: LIVE recon done, GATE still open, handed to Linux

Continued the native-enemy-name plan with **live external-RPM recon** on the running game (Windows box,
user-launched ER 2.6.2.0, in-world at Gatefront, FR). Findings →
`docs/re/windows_enemy_name_hud_feed_re_findings.md`. No code shipped; this is a handoff to the Linux
daily-build box (better here — in-DLL FWA proven + fresh DLL).

- **✅ Confirmed LIVE** (was offline `strings` only): `01_000_fe.gfx` field-HUD movie is loaded from the
  ACTIVE install (`MovieView`/`MovieData` + disk path `mod\menu\01_000_fe.gfx`); `EnemyTag_ColorText_12`
  is the name TextField (`/Text_0`); the 8 `EnemyTag0..7`+`M0EnemyTag0` instances are live. **★ The name
  is fed as Scaleform HTML via SetTextHTML** — found both the HTML source buffer
  (`<FONT LETTERSPACING='0'>Sentinelle de l'Arbre</FONT>`) and the UTF-16 TextField content, in the movie's
  heap arena. (All addrs session-specific — re-scan per boot.)
- **❓ GATE Q1 STILL OPEN** (behavioral): does VANILLA show the tag when fed? Not tested → do NOT delete the
  ImGui path yet.
- **🔬 Pause answer:** find-what-**writes** does NOT trigger while paused (the write instruction doesn't run;
  value is already resident). find-what-**reads** may (render still draws the frozen frame). So: pause =
  locate+arm; UNPAUSE + refresh the name = trigger a write capture.
- **🚧 Two small DLL gaps to fix FIRST (Linux):** `mem_fwa`'s write-watch already works (`make_dr7`
  `0b01`), but (1) there's **no disarm verb** so the single FWA slot wedged on a stale `@geom` probe this
  session → add `mem_fwa off`/force-rearm (host-only); (2) the hit log hardcodes "READ" even for writes
  (`goblin_field_probe.cpp:138`) → thread `write_only` to the label.
- **➡ NEXT (Linux):** land those 2 DLL gaps → rebuild/restart → near a NAMED enemy, re-scan the HTML-source
  buffer live → `mem_fwa <buf> 4 w`, UNPAUSED, refresh the name → `[FWA]` RIP + caller chain = the engine
  name-feed hook → author AOB. Then answer Q1 on vanilla.

## ⇒ SESSION WRAP 2026-07-06 (Linux/Opus) — enemy-name source SOLVED (=ERR Scaleform HUD) + font accents fixed; native-name plan queued

Long debug of the "enemy name drawn twice" report. Landed fixes + a scoped plan; local master ahead of origin.

- **The "second mod" = ERR's Scaleform HUD, not a DLL.** Full mod audit (`err_offline.me3` load list + strings)
  + a mods-off isolation test (disabled all `dll/offline/` natives, kept ERR internals → name persisted)
  PROVED the stable red NPC name is drawn by ERR's field-HUD movie **`mod/menu/01_000_fe.gfx`** — symbols
  `EnemyTag1..7`/`M0EnemyTag0` (= our 8 `entityHpBars`) + `EnemyTag_ColorText` (the name field). ERR names
  only NPCs (nameId>0); generics stay blank. Ours (ImGui) was the white duplicate. `01_000_fe.gfx` is
  uncompressed (`GFX\x0b`) → strings-readable.
- **✅ Font accents FIXED (`d23b779`).** "Varre" vs ERR's "Varré": our ImGui default font (ProggyClean, added
  with full 0x20-0xFF range) SHADOWED the merged DejaVu at 0x00A0-0x00FF (ImGui merge = first font wins) and
  ProggyClean has no real accented glyph → 'é' blank. (œ U+0153 worked only because it's outside the overlap.)
  Restricted ProggyClean to ASCII (0x20-0x7E) via a custom GlyphRanges on AddFontDefault → DejaVu owns every
  accent. Fixes accents overlay-wide (item/region/enemy names, all langs). Host-side → needs a game restart.
- **Enemy-name encoding is fine** — `wide_to_utf8` uses `WideCharToMultiByte(CP_UTF8)` (proper UTF-8). It was
  purely the font atlas.
- **The tier-1 skip (`803f887`) was reverted by the user (`c33bfe3`)** — do NOT re-add it blindly. The chosen
  direction supersedes it (below).
- **★ CHOSEN DIRECTION (user, 2026-07-06): go NATIVE — RE the GFx HUD, inject the enemy-name tag for ALL
  entities (mobs/NPCs/sheep), then DELETE our ImGui enemy-name path.** Scoped in
  `docs/plans/native_enemy_names_scaleform_plan.md`. **★ RE-GATE FIRST (mod-agnostic prime directive):** does
  vanilla's `EnemyTag_ColorText` DISPLAY when fed a name (→ native OK, delete ImGui) or is it HIDDEN for
  non-boss (→ shipping a gfx replacement = mod-specific/HUD-conflict = keep ImGui, instead fix its stability
  via per-frame w2s + drop the `kClampL/R/T/B` clamp). Next RE = the name-feed hook near CSFeMan+entityHpBars;
  write `docs/re/windows_enemy_name_hud_feed_re_findings.md`. Game was DOWN this session (no live probe).
- **Also queued (independent):** ImGui enemy-name STABILITY fix (per-frame w2s + drop clamp) — the fallback if
  the native path isn't mod-agnostic, and a quick win regardless.
- **★ PAUSE resume-latency BUG (live-confirmed 2026-07-06) — RE-gated.** The branch-flip pause
  (`goblin_pause.cpp`) hitches on RESUME proportionally to how long it was paused → the flipped branch gates
  the world STEP but not the CLOCK, so wall-time accrues while paused and the game drains the backlog on
  resume (confirms it's a partial/per-subsystem gate, not a central world-tick). Fix = zero the timestep.
  **★ RE PLAN WRITTEN: `docs/re/game_timestep_freeze_re_prompt.md`** — anchored on the ALREADY-RE'd per-frame
  driver `FUN_140623410` (er+0x623410: takes `float dt` → wraps in FD4Time → drives every subsystem). RE ~70%
  done; remaining = find the dt source (arg vs a global time-scale), zero it while paused → clean freeze +
  instant resume + a free `set_timescale` dev lever. Reworks `goblin::pause` off the raw branch flip. Interim:
  short pauses / keep "pause on open" off (already default).
- **★ ENEMY-NAME native-Scaleform RE — Windows agent live recon DONE** (`windows_enemy_name_hud_feed_re_findings.md`):
  `01_000_fe.gfx` + `EnemyTag_ColorText_12/Text_0` confirmed loaded live from the active install; name fed as
  **Scaleform HTML via SetTextHTML** (`<FONT LETTERSPACING='0'>name</FONT>`). GATE Q1 STILL OPEN. Linux handoff:
  capture the name-feed WRITE site (`mem_fwa <html_buf> w`, UNPAUSED). **★ SMALL UNBLOCK NEEDED (host-only): add a
  `mem_fwa off` disarm verb** — the single FWA slot wedges on a stale probe; `field_probe::disarm()` exists but no
  RPC reaches it. Also fix the hit-log WRITE/READ label (`goblin_field_probe.cpp:138` hardcodes "READ").
  `goblin_debug_rpc.cpp` + `goblin_field_probe.cpp`, no host↔render boundary.
- **★ Track A M1 + gamepad landed this session (see `imgui_only_map_plan.md`):** heightfield cast-window
  widen + sea-tag plumbing (`52c9fa4`); vmap gamepad canvas control — stick pan/zoom + virtual-cursor reticle
  (`4933ea2`, render-side/hot-reloadable). Font accent fix (`d23b779`). Gamepad OPEN findings queued in M4:
  need an on-screen keyboard for item-search + wire the reticle into hover/click/place.

## ⇒ SESSION WRAP 2026-07-05 (late-4, Linux/Opus) — PRIORITY: revive overlay hot-reload (split drifted) + boss/grace bugs queued

User wants to STOP the ~3-4min game-reboot per fix and revive the overlay hot-reload dev loop. Investigated
+ queued two marker bugs. NOT yet coded (diagnosis only this pass).

- **★★ OVERLAY HOT-RELOAD RESYNC = DONE + LIVE-VALIDATED 2026-07-05 (`73c8d96`).** The whole split
  drift below is FIXED: `build-linux-hotreload` links BOTH `MapForGoblins.dll` + `goblin_overlay_render.dll`
  clean, AND the default single-DLL build stays clean. **Live-proven:** booted the hotreload build
  (`status hotreload=1 gen=0`), exercised the moved surfaces (`vmap fit`/`offmap`/`find`, `f1_tab`,
  `far_relief_probe` — all ok), then `reload_overlay` → **`gen=0→1` (render module hot-swapped, no restart)**,
  the vmap dispatch still worked post-swap, `ping`→`pong`, and a screenshot shows the F1 panel + minimap
  render cleanly at 39fps. **DEV LOOP (no game reboot per fix):** edit a RENDER file (`panel_*`,
  `map_entry_layer.cpp`, `map_renderer.cpp`, `grace_layer.cpp` — NOT host files like `goblin_legacy_fold.cpp`)
  → `ninja -C build-linux-hotreload goblin_overlay_render` → copy `goblin_overlay_render.dll` to
  `dll/offline/` → watcher auto-swaps ~1.3s (or `reload_overlay` RPC) → re-test. Boot ONCE with BOTH DLLs
  deployed (`ninja -C build-linux-hotreload MapForGoblins goblin_overlay_render` + copy both). **Normal-play
  deploy = the DEFAULT build** (single DLL, `build-linux`); the offline dir is restored to it now (stale
  render DLL removed so the default host never half-loads it). **⚠ MAINTENANCE GUARD:** the split has no CI —
  every new host↔render call re-breaks it. Before finishing a session that touched the host/render boundary,
  run `ninja -C build-linux-hotreload MapForGoblins goblin_overlay_render` and fix any new undefined (move
  the file to host + `GOBLIN_RENDER_API`, or add a loader export — pattern in `goblin_overlay_render_loader.cpp`).
  The detail below is the (now-resolved) diagnosis, kept for the pattern.

- **★ Overlay hot-reload EXISTS + was Linux-validated (2026-07-02, `overlay_hot_reload_playwright_plan.md`)
  but the split-build LINK is now BROKEN — drifted since.** `GOBLIN_OVERLAY_HOTRELOAD=ON` builds the draw
  layer as a swappable `goblin_overlay_render.dll` (watcher auto-swaps ~1.3s, no restart; `reload_overlay`
  RPC forces it). `build-linux-hotreload` exists. But `ninja MapForGoblins` there fails: **20 undefined
  symbols** — host code added after 2026-07-02 calls render-side functions directly. TWO fix categories:
  - **A — move to HOST + export** (stateful data/logic misfiled into render): `goblin_virtual_world.cpp`
    (vworld registry — MUST be host so state survives reload), `goblin_add_collision.cpp`, `worldmap/
    maptile.cpp`. Mark their public API `GOBLIN_RENDER_API` (= dllexport from host, `goblin_dll_export.hpp`)
    so render imports via the `MapForGoblins.lib` it already links.
  - **B — add loader exports** (defined in `map_entry_layer.cpp`/panel which STAY render = hot-reloadable):
    `worldmap::rebuild_markers`, `build_far_relief`, `far_relief_probe`, `death_marker::{set,clear,tick}`,
    `overlay::panel::virtual_map_service_pending_warp`. Host calls these → route through the loader's
    GetProcAddress export table (7 fns; std::string returns cross the /MT boundary OK via the host-heap
    override `goblin_render_new_override.cpp`, proven for loot_disk in the 2026-07-02 validation).
  - **Payoff:** the frequently-edited files (`goblin_legacy_fold.cpp`, `panel_virtual_map.cpp`,
    `map_entry_layer.cpp`, `map_renderer.cpp`, `grace_layer.cpp`) are ALL render-side → hot-reloadable once
    resynced. Loop = rebuild ONLY `goblin_overlay_render` → copy DLL to `dll/offline/` → watcher swaps →
    `refresh_markers` → re-test via RPC. Needs ER kept alive across cycles (launch detached, drive one-shot
    RPC). **Full list:** `ninja -C build-linux-hotreload MapForGoblins 2>&1 | grep 'undefined symbol'`.
  - **⚠ maintenance root cause:** the split has no CI/build check, so every new host↔render call silently
    breaks it. After resync, add a `build-linux-hotreload` compile to the dev checklist / a VSCode task.
  - **★ RESYNC IN PROGRESS (2026-07-05, partial — DEFAULT build stays clean throughout):** Category A DONE
    (moved `goblin_virtual_world.cpp`, `goblin_custom_markers.cpp`, `goblin_add_collision.cpp`,
    `worldmap/maptile.cpp` to HOST_SOURCES + marked their public APIs `GOBLIN_RENDER_API`). Category-B first
    tranche DONE (loader exports `MFG_RebuildMarkers`/`MFG_ServicePendingWarp`/`MFG_BuildFarRelief`/
    `MFG_FarReliefProbe` + `call_*` wrappers in both branches + routed the 5 host callers). **But the build
    revealed the drift is ~2× bigger:** the ENTIRE `vmap` RPC→panel surface is called directly by the host
    (debug_rpc) — ~18 more `goblin::overlay::panel::virtual_map_*` fns (fit/group/tile/tiles_clear/load_lod/
    load_resident/tile_recon/locate/offmap/find/item_search/force_spiderfy/set_relief/set_view/set_flip/
    group/open) + `overlay::request_f1_tab`, `panel::dump_markers_csv`, `worldmap::far_relief_snapshot`, and
    more surface after those. **DECISION NEEDED — two ways to finish:** (1) per-function loader exports (same
    mechanism, ~100 more mechanical edits); OR (2) **ONE generic `MFG_VmapCommand(subcmd, args, out, cap)`
    export** — move the debug_rpc `vmap`-verb dispatch into the render module (it already holds all the panel
    fns), collapsing ~18 exports into 1 (much less wiring, but refactors the rich per-verb arg parsing in
    debug_rpc). Recommend (2) for the vmap surface + per-fn for the ~3 stragglers. **All committed as a WIP
    checkpoint; the default single-DLL build (the shipped/used one) links + runs unaffected.**
  - **★ CHOSEN = generic dispatch + DE-RISKED (2026-07-05).** Verified **15 of 16 `virtual_map_*` are
    DEBUG_RPC-ONLY**; only `virtual_map_locate` is also render-called (`panel_search.cpp`, itself render → can
    call `panel::virtual_map_locate` directly). The debug_rpc `vmap` block (`goblin_debug_rpc.cpp:501-651`)
    calls ONLY `overlay_api::virtual_map_*` (panel forwards) — no host-only deps. Clean continuation:
    (1) add `std::string vmap_rpc_command(const std::string &rest)` in `panel_virtual_map.cpp` = the 501-651
    body with `overlay_api::virtual_map_` → local `virtual_map_` (all defined in that file; watch name deltas:
    `is_open`→`virtual_map_open()`bool&, `dump_markers`→`dump_markers_csv`) + a local `next_token`;
    (2) export `MFG_VmapCommand(rest,out,cap)` + `call_vmap_command` (both loader branches, like the 4 done);
    (3) debug_rpc `vmap` block → `return …::call_vmap_command(rest);`;
    (4) DELETE the 15 debug-only `overlay_api::virtual_map_*` wrappers + `..._api.hpp` decls; repoint
    `panel_search.cpp` locate → `panel::virtual_map_locate`;
    (5) then per-fn loader-export the remaining stragglers the link surfaces (`overlay::request_f1_tab`,
    `worldmap::far_relief_snapshot`, + re-build to flush the next wave — errorlimit capped display at 20);
    (6) build BOTH, iterate to clean link, deploy both DLLs, boot once, validate `reload_overlay` (gen++,
    no crash). Keep the DEFAULT build green at every commit.
- **Boss "dedup" (task #16) — NOT A BUG, diagnosed 2026-07-05.** We faithfully render EVERY
  `WorldMapPointParam` boss row (`build_live_bosses`, `textId2==5100`) — no dedup drops instances. `[BOSSDIAG]`
  live dump proved `WorldMapPointParam` has EXACTLY 4 "Erdtree Avatar" rows (all BUILT, zero skipped) + 1
  GROUPED "Demi-Human Chief**s**" row. The native ER map is itself selective (marks 4 of the 7 Avatars) and
  groups the Chiefs into one icon — we match it. Showing all boss INSTANCES (7 Avatars, un-grouped Chiefs)
  would be a FEATURE: source bosses from enemy placements (the loot enemy-join already resolves them — e.g.
  it found 13 Night's Cavalry entities) + dedup + name-resolve. **★ USER WANTS IT (2026-07-05): "7 Avatar but
  item searcher shows 4".** Confirmed BOTH baked sources have only 4 Erdtree Avatars (`WorldMapPointParam` +
  `data/boss_list.json`, same 4 area60 locations grid 43,33/33,43/38,48/52,56, model c4810); the missing 3 —
  incl. the UNDERGROUND one — are absent from both, so they must come from the LIVE MSB enemy scan
  (`disk_enemies`: model in the part `name` e.g. `c4810_9000` + `npcParamId` + pos, all instances). **FEATURE
  PLAN (enemy-sourced boss supplement — mod-agnostic/runtime, NOT started):** (1) boss-model set — prefer a
  RUNTIME signal (NpcParam boss flag / defeat-flag bind) for mod-agnosticism, else the vanilla model
  whitelist from `boss_list.json` as an additive fallback layer; (2) scan `disk_enemies` for those models;
  (3) resolve each model→name (`npcParamId`→NpcParam.nameId→FMG, or `data/npc_name_text_map.json`:
  904810600/601/602="Erdtree Avatar"); (4) dedup vs `WorldMapPointParam` boss markers by position (a c4810
  near an existing "Erdtree Avatar" marker = same boss); (5) emit uncovered ones via `push_marker`
  (Source::Live, WorldBosses). Add to `build_live_bosses` in `map_entry_layer.cpp` (render→hot-reloadable).
  **✅ DONE + LIVE-VERIFIED 2026-07-06 (`352409c`):** `build_live_bosses` now supplements each marked boss
  type from `g_parsed.enemies` via the new `goblin::enemy_display_name` (tier-1/3 resolver, GOBLIN_RENDER_API
  exported), name-matched (plural-tolerant) + tile-deduped, reusing the marked row's textId1/iconId.
  `vmap find "Erdtree Avatar"` 4→**6** (incl. `area12` UNDERGROUND + `area11` Leyndell-folded); 223 instances
  supplemented across 162 boss types. Both builds link clean. **KNOWN LIMITATION:** Demi-Human Chief stays 1
  — its c4120 enemies don't resolve to exactly "Demi-Human Chief" (a name-resolution edge, not the plural
  logic). **`boss_list.json` DELETED (`69c9746`)** — it was WMP-derived (inherited the same 4), fed only dead
  outputs; removed the file + generator + diagnostic + pipeline stage + all refs. (Also re-proved
  the hot-reload loop here: added `[BOSSDIAG]` to `map_entry_layer.cpp`, hot-swapped render `gen0→gen1` live,
  read the log, removed it — no reboot. Persistent-game harness caveat: the keepalive loop exited when
  foreground `mfg rpc` calls raced its `ping` — gate the keepalive on real liveness, don't break on one ping.)
- **Grace story-flags (task #17) — RESOLVED 2026-07-06: DON'T gate.** Verified at the code level: graces are
  sourced LIVE from `BonfireWarpParam` (`grace_layer.cpp:31`, no bake) + carry a `discoverFlag`, drawn
  state-aware (discovered=warpable, undiscovered=helper). The live source is STATIC (never deletes old-state
  graces); the game gates availability by the discovery flag, which already reflects state. And grace NAMES
  already distinguish state ("Leyndell, Royal Capital" vs "…, Ashen Capital" = separate grace names → the
  item search shows them as distinct rows WITHOUT a story tag). So wiring story-flags into graces would be
  redundant + risks hiding valid graces = "gate for nothing". Royal/Ashen tagging stays LOOT-only (done).
  The earlier "graces untagged" note is the correct end state, not a gap. (Unverifiable without an in-game
  pre/post-burn comparison: whether a DISCOVERED Royal grace lingers post-burn — but hiding it needs the very
  gate we're declining, so leave as-is.)

## ⇒ SESSION WRAP 2026-07-05 (late-3, Linux/Opus) — off-map "bottom-left" root causes + fixes

User spotted a recurring symptom: **every off-map marker renders bottom-left**. Diagnosed (via the new
`vmap find` + `proj`) as TWO independent projection-failures that both collapse to ~origin, and fixed the
big one. All committed, in-game verified, local master ahead of origin.

- **★ Fix 1 DONE (`ac802b6`) — Leyndell Ashen Capital / Elden Throne fold gap.** Map areas 19/34/35 (Ashen
  Capital, Elden Throne) are DST dead-ends in `WorldMapLegacyConvParam` (no outgoing row) → `legacy_fold`
  returned unmatched → markers stayed at raw grid(0,0) = bottom-left (Elden Beast, Fractured Marika, Stakes
  of Marika, Summoning Pools, Hallowed Avatar). Fix: `goblin_legacy_fold.cpp` now builds a reverse index
  (dst_area→rows) and lifts a dead-end area into its parent block (11,5,0) via the row's inverse, selecting
  the parent's EXACT block so the terminal-preferring lookup routes it straight to the overworld (11→60) —
  a naive lift ping-ponged 19→11→19 (net-zero) and got worse (snapped to (0,0)); the exact-block select
  fixes it. Live: Elden Beast w(0,0)→w(11751,12505)=Leyndell; `vmap offmap` 20→15. The engine converter
  already knew this (`proj 19 0 0` == `proj 11 5 0`); affine confirmed **worldX=u+7040, worldZ=16512−v**.
- **★ Fix 2 NOT DONE — Night's Cavalry armor enemy-join mis-anchor.** The 4 armor lots (1048550710-713)
  resolve to area60 grid(12,13) = off-map-west, while the 13 Night's Cavalry BOSSES are placed correctly
  (the Duo at grid(48,55) = Mountaintops = where the armor drops). So it's an enemy-join failure: the armor
  lots don't join to the Duo boss entity and fall to a degenerate low-grid anchor. This is the residual
  `from_fallback` class (`vmap offmap` still flags 15 margin: Night's Cavalry set + area45/-1 quest NPCs).
  NEXT: trace why lots 1048550710-713 mis-anchor (loot_disk enemy join) → co-locate them with the Duo boss.
- **Item search Royal vs Ashen Capital DONE (`ac802b6`) — but graces untagged.** State-gated LOOT items now
  split into per-state rows tagged `[+] Royal Capital`/`[x] Ashen Capital` (reachability read live via
  `read_event_flag`, generalises to charm-break/seal-tree). **Follow-up:** graces bypass `push_marker` so
  they don't carry `secondary_flag`/`hide_when_flag` → grace rows are untagged; wire the story flags into
  `grace_layer` (call `secondary_story_flag`/`hide_when_story_flag`) to tag Ashen vs Royal graces too.

## ⇒ SESSION WRAP 2026-07-05 (late-2, Linux/Opus) — A15 CLOSED as parity + on-canvas icons already shipped

Scoping/bookkeeping pass, no runtime work. Two parity rows reconciled against the live code:

- **vmap on-canvas ICONS = already SHIPPED** (the "next brick" the prior wrap suggested). `panel_virtual_map.cpp`
  already draws real category icons on the canvas: base ER via `draw_marker_glyph` (state-aware native/atlas),
  custom worlds via `resolve_category_icon`/`icon_for` + `AddImage`, `s_show_icons` toggle, colored-dot
  fallback when no glyph resolves. Corrected the stale `:1302` comment ("on-canvas icons are a follow-up").
- **★ A15 legacy-dungeon sub-maps = CLOSED as parity (user decision).** Scoped it: vanilla ER has NO
  in-dungeon detailed map for legacy dungeons — the native map ALSO folds them onto the overworld as points.
  The vmap folds via `WorldMapLegacyConvParam` (`legacy_fold.cpp`) AND draws the dungeon's interior markers at
  the fold point → MEETS/beats native. Separate per-dungeon PAGES would be a mod feature BEYOND native, and
  doing it properly IS the dimension-registry pivot (`virtual_world_multi_world_design.md` L172-214:
  groups→mapId dimensions) + dungeon sub-map tile art (blocked on A3-tiles RE) — NOT a parity gate. Plan A15
  row + build-list updated (`imgui_only_map_plan.md`).
- **⇒ NEXT ungated Linux brick** (A15 no longer a candidate): `vmap offmap` extend to catch UG/DLC (0,0)
  render-side via `vmap_proj`; OR `active_world` auto-set from PlayerDim (the `s_group` follow half is done);
  OR the RE-track ADD-AEG render gap (request accepted, not yet a rendered instance). Parity gate now: only
  A3 tiles (Windows RE) + A10 fast-travel (Track C0) remain as real blockers; A12 = ERR-only low-prio.

## ⇒ SESSION WRAP 2026-07-05 (late, Linux/Opus) — M5 native-cull: BOTH cheap levers DISPROVEN + greybox #2a prompt

All committed (local master ahead of origin; USER pushes). Worked the vmap migration's M5 (native-map cull) and
retired a roadmap RE question.

- **greybox job #2a RE prompt WRITTEN** — `docs/re/windows_debug_render_flag_re_prompt.md` (`837199e`): a cheap
  Windows GO/NO-GO scan for a FromSoft debug-render flag (wireframe/untextured/collision) that restyles the
  engine's OWN render. Scoped explicitly as #2a only (NOT the ImGui-mirror #3 wall). Indexed in `re/README.md`.
- **★ M5 native-map cull — BOTH cheap Linux levers DISPROVEN live (`4d54094`).** Ran the recon end-to-end:
  - **D3D12 `RSSetScissorRects`** (was the RECOMMENDED lever): 0 mapopen=1-only rects — the Scaleform map draws
    full-screen through generic engine scissors, no map-specific rect to empty (tagging proven sound: the
    minimap correctly isolated as mapopen=0-only). Findings §4c.
  - **GFx `MovieImpl+0xB0` clip write** (the pivot): resolve corrected (2-deref `WorldMapDialog+0x140 →
    *(+0x00)`, validated `buf==1920×1080`); the zero-write HOLDS but the map still renders → `+0xB0` is
    descriptive, not a render gate. Findings §4d. Shipped `movieclip read|hide|show` RPC (`read` = live
    map-viewport diagnostic; hide/show INERT for cull, kept as scaffolding).
  - **⇒ M5 DEPRIORITIZED:** the cull now needs the Scaleform draw-vfunc no-op (Windows Ghidra), a movie
    visible-flag (risky RPM spike), or the CSMenuMan draw-skip — all not-cheap, and the production flip is gated
    on Track A parity + Track B fast-travel anyway. **Keep advancing the vmap migration on UNGATED bricks;** do
    the cull from the Windows/RE track when it has a slot. Plan M5 row + `imgui_only_map_plan.md` C1 updated.

## ⇒ SESSION WRAP 2026-07-05 (evening, Linux/Opus) — relief v0, search-marks, offmap fix, greybox design + Windows prompts

All committed (local master well ahead of origin; USER pushes). A long Linux session: shipped the far-terrain
relief v0, several vmap features/fixes, closed the Leyndell off-map bug, and locked a big chunk of the
runtime-modding-render ARCHITECTURE (with 2 Windows RE prompts written).

**⭐ NEXT SESSION (updated 2026-07-05 late): the M5 native-map cull is DEPRIORITIZED** — both cheap Linux levers
(D3D12 scissor + GFx MovieImpl clip) are live-disproven (see the late-session wrap above); the cull now needs a
Windows-Ghidra draw-vfunc pass, and its production flip is gated on parity+fast-travel anyway. **So continue the
vmap migration on an UNGATED Linux brick** — candidates: on-canvas ICONS for vmap markers (dots→glyphs, visible
quality win); extend `vmap offmap` to catch UG/DLC (0,0) render-side; A15 legacy-dungeon sub-maps (last
pure-Linux parity gap); `active_world`/`s_group`/PlayerDim auto-follow. **Meanwhile the Windows/Ghidra agent
runs** — priority `windows_world_to_screen_camera_re_prompt.md` (w2s3d = unblocker for the ImGui virtual world),
then optional `windows_havok_vdb_standup_re_prompt.md` + the new `windows_debug_render_flag_re_prompt.md` (#2a).
The tracks are independent.

**SHIPPED this session (all committed + in-game verified where noted):**
- **Baked `LEGACY_CONV` DELETED** — dungeon→overworld folds LIVE only (regulation param via `legacy_fold`);
  generator emission removed (`0afebbd`).
- **Catch-all `Loot - Other` category** — a resolved item with no taxonomy category is retained (not dropped),
  gated on `key>0` so phantom lots stay skipped; ~0 on ER, mod-agnostic safety net (`d8970c0`).
- **Relief D-far -1 v0** — MSB placement-Y cloud → per-cell median grid → `heightfield::Cell[]` → the vmap
  hillshade. Frame VALIDATED world-ish (`far_relief_probe`). Runtime-wired per group (auto-build on group
  change, 3 maps). Densified with ALL free sources (collectibles+treasures+enemies+regions+objacts+live
  graces). Renders a recognizable Lands Between (`5c569c7`/`d4253fc`/`f7572d9`). RPCs `far_relief`, `vmap relief`.
- **vmap search:** locate now pulses a ring at every hit (visible with markers off); item-search "MARK ALL
  RESULTS" (orange diamonds, capped, regenerated per search, Clear) drawn on vmap AND minimap via a shared
  `goblin::search_marks` store (`de6fd9c`/`ec4d8e0`/`d31bcf3`). Zoom-aware icon/spiderfy size.
- **spiderfy DX:** Ctrl-gated open (config `spiderfy_hold_ctrl`, anti pan-pop), no-steal by a neighbour,
  hint hidden while a fan is open (`c227cf3`/`ef588fc`/`1f91377`).
- **Off-map triage + FIX:** `vmap offmap` RPC (`virtual_map_offmap_probe`) → all 57 off-map = area-11
  (Leyndell) folding to (0,0). Fixed: `legacy_fold` prefers a TERMINAL-dst row per block over a dead-end →
  **0 off-map of 9796** (`b1f188c`/`e5ab521`).
- **STATUS.md regen rule** added to CLAUDE.md (it drifts; regen+commit after any RPC test run) (`23b543f`).

**DESIGN LOCKED (runtime-modding render — `runtime_modding_framework_vision.md` #4):**
- **Split by world type:** editing a real ER dimension → place existing **AEG** assets (engine-rendered,
  geom-spawn pivot 2); our **virtual worlds → always ImGui** greybox (Havok `add_collision` + ImDrawList),
  no mesh/MSB dependency.
- **Enemies = 3 layers:** visual (ImGui procedural, no skeleton) + behaviour (mod state machine) are doable;
  COMBAT (damage/hitreg) is the un-RE'd frontier → ImGui enemies = dummies until then; real actors = engine ChrIns.
- **ESP > Havok VDB** for collision-viz: ESP + reading hknp shapes ourselves = same result, in-game, no
  Havok-version lock. VDB machinery IS in the exe (`havok_vdb_presence_findings.md`) but SECONDARY.
- **THREE distinct "greybox" jobs** (don't conflate): (1) draw OUR objects = ImGui; (2) restyle the REAL
  base-ER render keeping systems = a GRAPHICS-PIPELINE hook (debug-render flag / post-process / D3D12 PSO
  wireframe), engine renders itself, NOT ImGui, FEASIBLE; (3) hide meshes + ImGui proxies = infeasible
  scene-mirror, unneeded. (The earlier "replace ER meshes = infeasible" was #3; #2 is a different doable track.)
- **The one real unblocker for the ImGui path = a 3D world-to-screen** (camera view-proj matrix) →
  `windows_world_to_screen_camera_re_prompt.md`.

**Followups queued (see Open items):** relief §6 filters (v0 = raw median, no outlier-reject) + the USER GATE
(evaluate v0 in-game → Havok bake or stop); extend `vmap offmap` to catch UG/DLC (0,0) via `vmap_proj`;
`active_world`/`s_group`/PlayerDim reconciliation for walkable worlds; a cheap "does ER have a debug-render
wireframe flag?" scan (for greybox-job #2 — **RE prompt now WRITTEN 2026-07-05:
`docs/re/windows_debug_render_flag_re_prompt.md`**, an independent Windows GO/NO-GO scan; #2a only, NOT the
ImGui-mirror #3 wall).

## ⇒ SESSION WRAP 2026-07-05 (Linux/Fable) — add_collision live, vmap projection/spiderfy/search, converter-residency

All committed + pushed (origin/master == HEAD `23a83f5`). Big Linux session on top of the Windows RE.

**Custom collision (Route D) — LIVE-PROVEN.** `goblin_add_collision.{hpp,cpp}` + staged `add_collision` RPC:
cinfo(defaults + shape@+0x00 + pos@+0x30, STATIC) → `allocateBody FUN_1418aabf0` → `addBody FUN_1418a9ff0(0,0)`
from the PRESENT thread (no deadlock), `hf_probe_present` oracle hit at the injected body's exact Y (Δfoot=40,
persistent). `test_add_collision.py` 9/9. Findings `hknpworld_addbody_slot_re_findings.md` §7. **Remaining:**
real `hknpBoxShape` build (`FUN_141916c30` + BuildCfg map — the probe BORROWS a live shape), walk-on confirm,
tile-re-stream persistence, hit-normal readback (was 0,0,0).

**AOB hardening (patch-resilience).** Last hot RVA + hknp FUNCs pinned: PHYSWORLD_SLOT (live FWA, primary
unique) + CINFO_INIT/ALLOCATE_BODY/ADD_BODY prologues → [SIG] 48/48 clean. Geom setter slot 26 already
self-heals. Backlog now: worldmap vtables/slots + icon-harvest set only.

**Virtual World Map — big progress toward replacing the native map:**
- **A9 item search DONE** — F1 result click locates onto the vmap (`virtual_map_locate`) AND the vmap has its
  own `Items` search sidebar (token-match, dedup rows, click=centre). Works native-map-closed. RPCs `vmap
  locate/items`.
- **Fork 1** — cluster PILES now honour region-name toggles (region_gated excluded at QT build + region mask
  in the rebuild key). Piles de-count hidden regions.
- **Fork 2** — underground/DLC markers re-projected through the LIVE engine converter (`worldmap_probe::
  project` → worldX=u+7040, worldZ=16512-v) instead of the baked fold that clumped them bottom-left (Nameless
  Eternal City etc.). Verified group-1 spreads under Deeproot/Ainsel/Siofra. Residual: a few area-12 tiles
  with no converter row still baked. NB the converter does NOT need the map open each time — `find_view_model`
  caches the VM which persists past close (proven, see below).
- **Fork 3 — spiderfy DONE** — native hover-fan ported to the vmap: piles (via `MarkerQuadtree::gather_pile`)
  AND exact/near-coincident singles (bucketed by screen cell — the case zoom can't separate). Ring/spiral +
  legs + dedup ×N + "+N" overflow. Fixes since: modal hover-absorb (no leak to icons under the fan), drops
  collected/cleared members when `collectedGraying` (churn ↓, action-only fan). `config::clusterSpiderfy`
  gate. Dev RPC `vmap spiderfy 1`.
- **A3 tiles (UG/DLC) — BLOCKED on Windows RE.** Live recon showed `harvest_resident_tiles` walks the wrong
  object — the tile tree is on the per-LOD DESCRIPTOR (`layer+0xd8`), NOT `layer+0x230` (outside the inline
  0x110 layer). RE prompt written: `windows_worldmap_tile_resident_reach_re_prompt.md`. `vmap tile_recon`
  correlation RPC ready for when harvest returns tiles.

**Converter residency — VERIFIED SAFE for the M5 cull.** The recommended native-draw removal (D3D12
`RSSetScissorRects` empty-clip, commit `2208332`) hides pixels but KEEPS the menu logic tick → VM stays live.
Proven weaker-than-close: `proj` RPC + `test_converter_residency.py` (5/5) → project() identical map-closed
(du=0.0). **Endgame data cleanup DONE 2026-07-05** (`imgui_only_map_plan` Track C): DELETED the baked
`LEGACY_CONV` table (`src/generated/goblin_legacy_conv.hpp`) + its nearest-base-point scan in
`project_dungeon_row_to_overworld` + the sibling scan in `goblin_markers.cpp::entry_world_coords` + the
generator emission (`tools/generate_data.py`). All dungeon/UG/DLC projection now folds LIVE via
`goblin::legacy_fold` off the resident regulation `WorldMapLegacyConvParam` (its own exact-block +
nearest-base-point lookup). The baked scan was already dead once regulation is resident — which every caller
is — so this was behaviour-neutral in steady state; warm-up window → raw/circle fallback. Cross-build clean.

**Parity gate (Track A) now:** A1/A2*/A4/A5/A6/A7/A8/A9/A11/A13 ✅ + gamepad nav + grace search/warp. Open
pure-Linux: A15 (legacy-dungeon sub-maps). A3 tiles = Windows RE. A12 (ERR dial) low-prio.

## ⇒ SESSION WRAP 2026-07-05 — custom-asset/collision RE + far-terrain relief plan (static Ghidra, Windows)

RE-only session (Windows Ghidra `D:\ghidra_proj2\ER`), all committed, local master ahead of origin. Landed:
- **Custom-asset creation options A–D** (`docs/re/custom_asset_creation_options_re_findings.md`): walkable
  greybox needs NO authoring (Route D = Havok `hknpBoxShape`); a new AEG id DOES stream (name-resolved VFS);
  B (buffer register) DEAD; C (primitive w/ collision) = the Hit layer is model-name-driven ⇒ collapses to D.
- **`hknpWorld::addBody` = a COMMAND, not a slot** (`docs/re/hknpworld_addbody_slot_re_findings.md`): full
  opcode map + recipe (`FUN_1418aabf0` allocateBody → `FUN_1418a9ff0` addBody) + `hknpBodyCinfo` layout
  (+0x00 shape/+0x28 motionType/+0x30 pos/+0x40 quat). **`add_collision` IMPLEMENTED + live-proven by the
  Linux agent** (9/9). Remaining: real box builder `FUN_141916c30`(AABB,radius,cfg) — probe borrows a live shape.
- **Far-terrain elevation SCOPED** (`docs/re/far_terrain_heightmap_re_findings.md`): NO heightmap texture
  exists; terrain = FLVER/`.mapbnd`; the walkable-ground truth = **`hkxpwv` map collision =
  `hknpCompressedMeshShape`** (verified §5b). `WorldMapPieceParam` ruled out (2D reveal rect, no Y).
- **PLAN: `docs/plans/far_terrain_relief_plan.md`** (far-field companion to `heightfield_relief_plan.md`),
  **v0-first + USER GATE**: first brick = **D-far -1 = FREE relief from the already-parsed MSB placement Y**
  (`loot_disk` keeps `posY`; the ERR log shows ~480K asset + 26K enemy placements — the "clutter" pots/jars
  are the BEST ground-Y), filtered anti-false-relief (layer-sep + vertical-outlier reject + per-cell median +
  type weight). Ship v0 → **if the user judges it sufficient, STOP** (the offline `hkxpwv` collision bake
  D-far 0 stays UNBUILT/gated). Followup = authored Y for custom `.toml` objects. **Not started.**

## ⇒ SESSION WRAP 2026-07-04 (evening) — custom markers + death marker + minimap edge fix

All committed, local master ahead of origin. Full status: `docs/plans/custom_markers_plan.md`.

**DONE + verified (screenshot):**
- **Custom player markers** (`src/goblin_custom_markers.{hpp,cpp}` shared store, UI in `panel_virtual_map.cpp`):
  right-click vmap = place blue pin (on top); `Custom` sidebar = list per-marker map+coords, rename, **Go**
  (pan), **Delete** (+ right-click-pin delete); cap 24/world; drawn on minimap too. **TP button HIDDEN**
  (coord-teleport not an ER mechanic; `warp_to_world_xz` bridge kept for future streaming warp = Track B).
- **Minimap edge-clamp fix** (`map_renderer.cpp draw_minimap`, `08e0548`): one `edgeR = R-half-2` → icons +
  custom + search-clamp all sit inside the border ring, no overshoot.

**IN PROGRESS — death/bloodstain marker (`37654c4`):**
- Icon name CONFIRMED = **`MENU_MAP_DropSoul`**. Render DONE: `goblin::death_marker` store + draws the native
  DropSoul via `map_point_glyph_uv` (disk-resolved, CACHED, mod-agnostic, red-disc fallback) on vmap+minimap.
- Trigger = RPC `death_mark` / `death_clear` (MANUAL only). **⚠ auto death-detect NOT wired** → in-game death
  renders nothing (user tested, "zero render" = expected). **NEXT: player-HP reader** — `LocalPlayer=[WCM+
  0x1E508]`, pos +0x6C0, yaw +0x6CC; **HP offset UNKNOWN → runtime probe** (take damage, find the int byte;
  candidate ER chain `LocalPlayer→0x190→…→HP`). Fire death_marker::set(get_player_map_pos) on HP→0 edge.
- **RE-VERIFY the render** first: `death_mark` then vmap/minimap should show the DropSoul sprite (or red disc
  if `map_point_rect_by_name("MENU_MAP_DropSoul")`/SB_MapCursor_02 disk-load fails). Last screenshot was too
  cluttered to confirm; isolate with `vis master 0` + tight zoom on the marked spot.

**Queued (this feature's follow-ups, `custom_markers_plan.md`):**
- Native PLAYER cursor (replace red arrow): `MENU_MAP_Player_02` (effigy) + `MENU_MAP_Bearing` (arrow),
  ROTATED by yaw via `dl->AddImageQuad` (4 rotated corners; yaw already read; calibrate a +π/2 offset).
- Followup 2 multiplayer icons — native names found: `MENU_MAP_Host/Guests/Coop_01-02/Friend_00-03/Enemy_00-03/Raid_01-02`.
- Custom markers Base-ER only (bug) → per-vworld. Persistence `custom_markers.toml`. SHIP RULE: custom
  content must stay marker-mapper compatible. **#3 delete ER compass** (Scaleform HUD, separate RE) still pending.
- Extracted native textures for icon-hunting: `tools/extracted/` (SB_* sheets).

## ⇒ SESSION WRAP 2026-07-04 (later) — RE-tooling hardening + player teleport harness + dev-dimension direction

**⭐ PRIORITY DECISION (confirmed 2026-07-04): Track 1 = the ImGui-only map** (`docs/plans/imgui_only_map_plan.md`,
now phased **M1→M5**, M3 = usable/ship line) comes FIRST. **Track 2 = runtime-modding RE** (dev-dimension/
teleport, MSB-write, ESD, weapon-arts — item 3 below) is the ORTHOGONAL follow-on, NOT worked until Track 1
ships. The teleport harness (below) stays as a ready Track-2 asset + it doubles as the M2 heightfield
warp-accumulate tool.

All committed (local master ahead of origin). Three things landed + a strategic direction set.

**1. RE-tooling hardening (patch-resilience).**
- **`tools/hf_hook_scout.py`** — live-RPM scout for the D2.2 safe-hook (subcommands `slot`/`fwa`/`disasm`;
  the remote branch then added `--aob` and used it to AOB-harden the 3 hot-path FUNCs + 2 hooked grace fns).
- **Build fingerprint** — `goblin_build_id.hpp::er_exe_version()` (reads `eldenring.exe` VS_FIXEDFILEINFO)
  → `[BUILD]` boot-log line before the AOB health check + **`er_version`** RPC verb (twin of the new
  **`er_base`** verb). So "am I on the build my RVAs/AOBs are pinned to?" is verifiable. `patch_diff_maintenance.md`
  freezes the recovery recipe (BinDiff/Diaphora on the **decrypted dumps**, never the VMProtect'd on-disk exe).
  ⚠ needs a DLL rebuild+deploy for `er_base`/`er_version`/`[BUILD]` to exist.

**2. ⭐ Player teleport harness — the mod's FIRST player-pos WRITE (`d8adca3`).** RPC `coords` / `warp_local`
/ `warp_xyz`, all on the tile-local Havok frame `LocalPlayer+0x6C0/6C4/6C8` (the frame `get_player_world_pos`
reads and **er_console_mod's `tp`** writes). `write_player_local_pos` mirrors the read-probe noinline+SEH
shape; `warp_xyz` converts world→tile-local via the confirmed `world=grid*256+local` map. **NEXT = run the
streaming-gate verification IN-GAME** (needs deploy): (a) `coords` vs er_console `coords` → same `+0x6C0`
frame?; (b) `warp_local X Y Z` twice, same values → same spot = absolute-in-tile, drifts = pure delta;
(c) `warp_xyz` intra-region (should hold — a ~1500-unit hop already streamed auto per user) then push to a
far cell / dungeon / underground + probe with `hf_probe` → find where collision stops following = the
open-world-continuous vs cross-map-transition boundary.

**3. Direction set — "dev dimension" walking skeleton (the way to see what's missing for runtime modding).**
Survey of the RE frontier done (map mission's frontier in `docs/re/README.md` = MSB-write, terrain/collision-
write, ESD/EzState, regulation-VFS, FLVER; PLUS gameplay-modding pans OFF the map's radar: EMEVD-execute,
SpEffect-apply, damage-hook, live-enemy-spawn, anim/behavior, arbitrary-teleport, bullet-spawn, weather).
Weapon Arts = `EquipParamGem` (Ash of War, id nibble 0x8) + `SwordArtsParam` behavior — data driveable,
behavior/install NOT RE (same wall). **Plan idea (not yet written):** a TOML-declared dev dimension whose
*realizer logs each field it can't yet honor* → the missing-primitive checklist writes itself live in-game.
Key facts established this session: streaming-safe warps in ER are ALL anchor-based (grace id / EMEVD
warp-to-entity), there is NO native warp-to-XYZ; but a raw `+0x6C0` write DOES ride open-world continuous
streaming (er_console 1500-unit hop). Zero-inventory dev player ≈ free today (`strip_goods`/`restore_goods`).
**Open next:** decide whether to write `docs/plans/dev_dimension_walking_skeleton_plan.md` + scope the
streaming-gate / free-teleport question once the harness test above returns data.

---

## ⇒ SESSION WRAP 2026-07-04 — Virtual World Map (mod-owned page) + test orchestration

Big session. Two tracks landed (all committed, in-game verified where noted; local master ahead of origin):

**1. Virtual World Map — the mod-owned map page (World Virtualization vision #1).** Slices A→D + C1/C3 all
DONE + live-verified (see the "In-Game World Editor" item's virtual-page block below for detail):
A canvas (pan/zoom/grid), B markers (6837 ER markers → Lands Between silhouette), C1 world registry
(`goblin_virtual_world`), C3 bundle persistence (`virtual_worlds.toml`, 2-cold-boot verified), D decouple
from F1 + open via the game MAP KEY. RPCs: `vmap`/`vworld`/`f1_tab`. **NEXT on this track (pick one):**
- **ENDGAME phase-1a = load ER's real map ART** onto the canvas → `docs/plans/map_tile_loading_plan.md`.
  **✅ sub-slices 1a+1b DONE 2026-07-04** (format cracked + validated offline AND in-game): BHF4 parser
  lives in `src/worldmap/maptile.{hpp,cpp}` (`parse_bhf4`/`load_archive`/`extract_dds`) + `maptile_probe`
  RPC; offline tool `tools/tpfbhd_recon.cpp`. In-game recon of the packed `71_MapTile`: **28469 tiles,
  256×256**, extract chain works; 4 dimensions (M00 overworld / M01 underground / M10 DLC / M11 DLC-ug),
  LOD pyramid L0(fine)→L3/L4(coarse) — full per-level counts in the plan.
  **✅ sub-slice 2 DONE + LIVE-VERIFIED 2026-07-04:** the vmap canvas now draws REAL ER map ART. RPC
  `vmap tile <needle> [rect]` / `tiles_clear`; `maptile::extract_named` + `panel_virtual_map` service the
  load on the render thread (`create_tex_from_dds_mem`, like panel_dev_icons' on-click load), cache
  {SRV, world quad}, and `dl->AddImage` under grid+markers. A 2×2 M00_L0 block rendered as seamless ER
  terrain (screenshot-confirmed under Proton).
  **✅ sub-slice 3a DONE 2026-07-04 — live map-space→world transform (the Convergence-trap-safe core).**
  The vmap places tiles in the SAME world frame as markers, derived 100% LIVE via `worldmap_probe::project`
  (project every overworld marker → robust MEDIAN offset, fixed ±1 slope since converter scale is live=1):
  result EXACT `worldX=mapU+7040, worldZ=−mapV+16512`, ground-truth-verified (marker world(10138,10046) ↔
  engine map(3098,6465)), NO hardcoding. RPCs `vmap tiles_lod <dim> <lod> [cap]` (reads archive once,
  center-out, cap for the SRV limit) + `vmap view <camX> <camZ> <zoom>`. Tiles land in the correct region
  (co-located with markers, live-verified). **Testing gotchas found:** `set rpc_auto_idle false` before
  scripted input (auto-idle SUSPENDS it when a human is at the PC); the map cursor/VM only publishes once
  the map view is NON-static (pan/zoom); the ER world map opens with the **`m`** key on this install.
  **NEXT (3b/3c/placement):** (1) **Tile RECT/placement — SOLVED + calibration fixed (static Ghidra,
  2026-07-04, `docs/re/windows_worldmap_tile_rect_reach_re_findings.md`).** The engine grid is
  `gridX=clamp(floor(mapU/cs),0,N-1)`, `gridZ=clamp((N-1)−floor(mapV/cs),0,N-1)` **(Z axis FLIPPED, base 0)**,
  per-tier `cs/N={256/41, 342/31, 1288/9}` (overworld 256/41). Tile map-space rect `=(gridX·cs,(N-1-gridZ)·cs)`
  `+256×256`; `tileId=dim*10000+gridX*100+gridZ`. Calibration fn `FUN_1408849e0`, cell walk `FUN_1409d9ba0`.
  **This CORRECTS the first pass** (`..._tile_placement_re_findings.md`): the `col·64` morton name-grid + the
  missing Z-flip caused the live offset (name-gridX~64 vs the real N=41 grid). **The reliable MFG fix = read
  LIVE rects** (no name decode): walk `WorldMapArea`(vt er+0x2b2cb08, layers `+0x390` stride 0x110) →
  `WorldMapTiledLayer`(tree `+0x230`) → `WorldMapTile{+0x30 tileId, +0x98 rect}`; that gives
  `(gridX,gridZ)→rect` authoritatively. Implement in `maptile.cpp` (ref C in the findings) + confirm live on
  Linux. `suffix=8·morton` only describes the archive FILENAME (texture-fetch = deferred). (2) **SRV
  recycling** — 256-cap no free list; even coarsest M00_L3=561>256. (3) byte-range reads (extract reads whole
  1.26 GB .tpfbdt). Then the full seamless overworld renders under the markers.
- Missed design items (captured in `docs/plans/virtual_world_multi_world_design.md`): **GAMEPAD** for the
  vmap canvas (stick→pan/zoom + reticle — add to every vmap slice); the real feature gaps = **clock /
  blue click-marker / custom beacon** (rest of ER's map is cosmetic; grace = fast-travel = make-or-break).
  **✅ sweep coverage DONE 2026-07-04:** `test_vmap.py` (single-boot SWEEP: open/close/group/fit round-trip,
  10/10), `test_vworld.py` (2-boot persistence: create+2 markers+save → cold boot → `vworld list` shows
  `(mk=2)` restored FROM DISK, 8/8), `test_world_bundle.py` (2-boot, the TOML-fix proof, 4/4). Added a
  marker-count readout to the `vworld list` RPC (`[id]name(mk=N)`) to make the reload verifiable.

**2. Test orchestration — regressions are no longer phantoms.** `mfg_session` persists PASS/FAIL to
`tools/rpc_tests/results.jsonl` (gitignored) + inline regression flag; `check_regress.py` scans + regenerates
git-tracked `tools/rpc_tests/STATUS.md`; `run_all.py` = AGGREGATED sweep (single-boot tests share ONE game
boot via a `SWEEP` marker → 9-test suite = 4 boots) → ledger → check_regress → gated exit. Nightly LOCAL cron
line documented (user adds to crontab; **Steam must be up** — `steam -silent`). `.vscode/tasks.json`
(git-tracked) = one-click build/deploy/test. Paths now **env-driven** (`.env`/`.env.local`, ERR_ROOT+GAME_DIR;
`.env.local` gitignored). `assets_probe` RPC + `test_assets.py` = path-loading guard (loose/packed/MISSING per
install shape). Open follow-up: `world_bundle` TOML load is latently broken under Proton (migrate to
TOML_EXCEPTIONS 0 like virtual_world/custom_items — see the open item below).

**Also this session (RE):** geom-spawn ADD standalone-ctor confirmed DEAD END (builder hangs, streaming-
welded); `spawn_clone` neutralized; real ADD = the asset-request path (pivot 2, Windows RE) — see the geom
placement item below.

## ⇒ RESUME HERE — sidecar Phase 2 (clean-save item strip/reinject): bracket is LIVE, cap-oracle E2E is next

**Where things stand:** the whole-slot save serialize is found and pinned — `SERIALIZE_FN`
@ `er+0x67dc00` (`FUN_14067dc00`, GameDataMan-xref ∩ DLOutputStream-writer, save-specific/synchronous/
direct-called; full RE in `docs/re/windows_save_serialize_re_findings.md`). `install_save_hook()` is
retargeted to it, observer-confirmed firing on the save worker thread (2 fires/save, correct AOB), and
the strip@entry/reinject@exit bracket is **wired + live** (`be7b212`): `strip_items()` →
`g_orig_ser(..)` → `reinject_items()`, synchronous, guarded by `g_in_serialize`,
`kItemStripReinjectWired=true`. `test_sidecar` 5/5 passes with the bracket live, no crashes.

**NOT yet proven:** that the bracket actually produces a clean on-disk vanilla save. Recipe: grant a
reserved-id item live (`give_item`) + register it (`sidecar additem`) → trigger a real game save →
reload with the `.mfg` `[items]` emptied → item must be GONE from the vanilla save. That assertion
needs an automated **`goods_count(id)`** read — now SOLVED:

**✅ `goods_count` FOUND + IMPLEMENTED 2026-07-03 (Windows-Ghidra, `docs/re/windows_goods_count_re_findings.md`).**
The blind 2-level `goods_diff` failed because the held qty is neither inline next to the id NOR ≤2 hops
out: ER uses GaItemHandle indirection AND the held list is a **two-segment split list three hops from
GameDataMan**. Ghidra (`D:\ghidra_proj2\ER`, new `tools/ghidra/find_goodscount.java` + `query.java`)
pinned the full layout: **`GameDataMan+8 → +0x2B0 EquipGameData → +0x158 EquipInventoryData` (carried)**;
segments (seg1_cap@+0x1C, seg1_base@+0x50, seg2_base@+0x40, last_index@+0x80), node stride `0x18`, node
`{handle@0 (0⇒empty), itemId@4 (0x40000000|goodsId), quantity@8}` (qty offset cross-checked via decrement
path `FUN_14024bfe0` + accessor `FUN_1407127a0`). Delivered **option (3), the direct read-only walk** (no
game call, no thread/save-timing risk) as `goblin::inventory::goods_count(id)`
(`goblin_inventory.{hpp,cpp}`, RPM-guarded, reuses `equip_game_data()`) + RPC `goods_count <id>` (reports
`err not in-world` vs a real `n=0`). Builds clean (clang-cl). Callable fallbacks recorded
(`FUN_14024c460`/`…c560` by-id finders).

**✅ goods_count offsets LIVE-VERIFIED 2026-07-03 (Linux/Proton).** Cross-built + deployed, then
`tools/rpc_tests/test_goods_count.py` (GameSession cold-boot → load save → grant/read) went 6/6:
fresh id `0x40003bed` reads `0→1→2→3` on repeated `give_item +1`; held id `0x40003bec` `7→8→9→10`.
Read tracks live held qty per-id, in-world. **Caveats found (give_item, NOT the read — full note in
`windows_goods_count_re_findings.md`):** AddItemFunc is ADD-ONLY (negative qty = no-op — the old
"−7 → 0" verify recipe was wrong; removal needs the remove path); `qty≥~5` clamps to the ~1000 stack
cap (grant N via N× `+1`); grants are live-inventory only, not persisted until a real save (fresh id
re-reads `0` after reboot → regression is idempotent).

**✅ Variant A clean-save CLOSED 2026-07-03 (Linux/Proton, E2E 4/4).** `tools/rpc_tests/test_custom_item.py`
(two cold boots: grant+additem+warp-save → empty `.mfg [items]` → reload) proves a registered custom
item does NOT survive in the vanilla `.sl2` once the `.mfg` stops re-granting it: boot-2 `goods_count==0`.
Three fixes made it work (the original bracket was a silent no-op):
- **Real strip (not `give_item(-qty)`).** AddItemFunc is add-only, so `strip_items()` now zeroes the
  matching EquipInventoryData node directly (`inventory::strip_goods()` — snapshot 0x18 bytes, write
  `handle@0=0`+`qty@8=0`; the exact decrement the game's `FUN_14024bfe0` does) and `restore_goods()`
  writes the bytes back the instant the serialize returns. The serializer honors the zeroed slot.
- **`WriteProcessMemory`-to-self silently FAILS on the inventory pages** (qty stayed 6 after a WPM
  strip) — a **direct in-process store under SEH** (`write_dw`/`write_bytes`) sticks. Use those, not WPM.
- **Idempotent reinject.** World-enter `reinject_items()` now grants only the missing delta
  (target − held) via the exact `give_item(+1)` primitive — a warp/area re-enter (item still live)
  grants 0 instead of inflating +1/save; a cold load (item stripped from the save, held=0) grants full qty.
Dev RPC `strip_test <id>` validates the strip round-trip WITHOUT a save (before→strip→0→restore→before).
Variant B (reserved-id item tolerated in the `.err`, no serialize hook) remains the zero-RE fallback.

**NEXT:** Gap C GRANT for arbitrary custom items can now build on this proven sidecar item (the Gap H
"don't dirty the `.sl2` until strip proven" contract is satisfied). Caveat still open: `give_item(+N)`
single-call is unreliable for N>1 (caps ~1000) — grant N via N× `+1` (reinject already does).

**Infra note (corrects stale memory):** a background Claude job CAN boot ER for a self-contained RPC
run — the missing piece was **Steam must already be running** (me3's `require_steam` aborts otherwise:
`ERROR require_steam: Steam is required to run this game`). Start it headless once with
`steam -silent` (auto-login persists, daemonizes, survives across tool calls), then `GameSession`
launches me3 as its in-shell child and kills the game at exit. See `mfg-rpc-driver-hardening.md`.

## Open / next items

- **★ 3D world-to-screen (camera view-proj matrix) — STATIC RECON DONE (2026-07-05,
  `docs/re/windows_world_to_screen_camera_re_findings.md`; prompt `..._re_prompt.md`).** THE single unblocker
  for the runtime-modding virtual worlds. Ghidra found: ViewProj candidates = `GameRend`/`GameRendCameraSet`
  instance **+0xF0 / +0x130** (two consecutive 4×4, default-init in `FUN_1406800f0` er+0x6800f0); alt VIEW
  block via `[[cam+0x10]+0x18]+0x10` (CSCam step er+0x76e7c0). Present hook (`hk_present`) + player-XYZ oracle
  (`LocalPlayer+0x6C0`) already in the DLL → overlay path ready. **NEXT = LIVE `w2s_probe` RPC:** pin the
  `CSCameraImp` FD4Singleton instance anchor + empirically confirm which block is ViewProj (row/col-major,
  NDC-Y) by projecting player pos → feet. (Needs the game running — Proton dev box.)
- **(old) 3D world-to-screen — Windows RE prompt written (2026-07-05,
  `docs/re/windows_world_to_screen_camera_re_prompt.md`).** THE single unblocker for the runtime-modding
  virtual worlds: read object world XYZ → project to screen → ImDrawList (the ImGui/ESP overlay path,
  `runtime_modding_framework_vision.md` #4). We have the 2D map w2s but NOT a 3D gameplay one. Target =
  `GameRendCameraSet` (er+0x680460, "consumes the final view matrix") / `CSCameraImp` — find the live 4×4
  view-projection + resolve chain (AOB), ship `w2s3d(xyz)`. Reuse the freecam recon (shares the camera
  subsystem; freecam WRITES the transform, this READS the ViewProj). Acceptance = project the player's world
  pos, the dot sticks to the character through camera motion. Unblocks: player greybox world + an in-game
  hknp collision wireframe (reuse the shape RE, no VDB version-lock).
- **Havok VDB present in ER — RE spike candidate (2026-07-05, `docs/re/havok_vdb_presence_findings.md`).**
  `strings eldenring.exe` confirmed the Havok Visual Debugger machinery is compiled in (NOT stripped):
  `hkSocket`(+Reader/Writer), `hk/hknpProcessContext`, `hknpViewer`/`hknpMultithreadedShapeViewer`,
  `hkbBehaviorServer` + `VisualDebugger\Server\...cpp`, and the `hkSignal2<hknpProcessContext, hknpWorld>`
  hook — and we already hold the live `hknpWorld` (`CSPhysWorld+0x08`). Standing up a VDB context+viewer+socket
  over it → connect the official Havok VDB client → a 3D view of the whole collision world incl. our
  `add_collision` bodies = a real DEV visualiser (verify collision/custom geometry/far-terrain). Main risks:
  version-matched VDB client + whether the server is fully linked vs partially stripped. Orthogonal to the
  player-facing ImGui/ESP greybox. **SECONDARY** (lean ESP — in-game hknp wireframe, no version lock). Spike
  prompt: `docs/re/windows_havok_vdb_standup_re_prompt.md` (settle the matched-client GO/NO-GO first).

- **✅ Off-map markers = area-11 (Leyndell) fold-to-(0,0) — FIXED 2026-07-05.** `vmap offmap` triaged ALL 57
  off-map markers → origin-zero, all raw area 11 grid(5,0) (Leyndell/Ashen graces + loot). Root cause: block
  (11,5,0) carries SEVERAL `WorldMapLegacyConvParam` rows with different dst (→60 overworld AND →19/34/35
  sub-maps); `legacy_fold` kept the FIRST per block (param row-id order), and a dead-end (area 19 = 0 further
  rows) won → the marker folded to grid~0 = (0,0). Fix (`goblin_legacy_fold.cpp ensure_built`): a row whose
  dst is a TERMINAL (overworld, area∈[50,88]) beats a non-terminal dst for the same block → (11,5,0) now folds
  to a60 (Leyndell overworld). Re-ran `vmap offmap`: **0 off-map of 9796** (was 57).
  **ALSO (user-observed 2026-07-05): (0,0) objects appear on ALL 3 pages (OW/UG/DLC), not just overworld.**
  `vmap offmap` only surfaced the 57 area-11 ones (they fold to group-0 OW) because it reads the STORED
  worldX (marker_world_pos / legacy fold). The vmap DRAWS UG/DLC via `vmap_proj` (the live VM converter), so
  UG/DLC markers the converter maps/fails to (0,0) draw at origin on those pages but are INVISIBLE to the
  fold-based probe. Followup: extend `virtual_map_offmap_probe` to ALSO triage the `vmap_proj`-projected coord
  per group (render-side, matches the actual draw) so the UG/DLC (0,0) get caught + attributed to their area.
- **vmap `active_world` / `s_group` / PlayerDim reconciliation — followup for WALKABLE worlds (2026-07-05).**
  The vmap gates relief / item-search / player-dot on `active_world == 0` (Base-ER data). Correct TODAY
  because custom worlds are marker-only so the player is always in real ER — but once a walkable custom world
  exists (reserved mapId), `active_world != 0` while the player is physically in it, so those gates must key
  off "engine-backed / owns ER-frame data" and PlayerDim (`get_player_dimension_area`, already RE'd) should
  auto-set the active world. **✅ The `s_group` auto-follow HALF is DONE 2026-07-05** (`Follow` toggle, default
  on: edge-triggered page switch to the player's dimension on a crossing; manual pick sticks between crossings;
  camera untouched. Live-verified: manual `vmap group 1` holds with Follow ON while the player is in OW = the
  edge-detect doesn't over-fire; the real OW→UG switch is user-verifiable by entering a cave). **REMAINING = the
  `active_world` half** — auto-set the active world from PlayerDim + re-key the relief/item-search/player-dot
  gates off "engine-backed" (not `active_world==0`) for when a WALKABLE custom world exists. Detail:
  `docs/plans/virtual_world_multi_world_design.md` Decision 2.
- **★ Mod manifest system (`mod.toml`) — SLICES 1+2 DONE + LIVE-VERIFIED 2026-07-05 (`b3a1114`, `bcaf829`).** One
  `<mod_folder>/mod.toml` declares + OWNS the whole-mod boot; `goblin::mod::load` (dllmain `init_mod`) realizes it.
  **Slice 1** = `[mod]` metadata + `[style]`→postfx at boot (verified: `[style] mode=posterize` restyled AT BOOT,
  no manual call). **Slice 2** = the manifest orchestrates `custom_items → world_bundle → vworld → style` in the
  required order, replacing the three standalone dllmain `init_*` calls with one `init_mod` (same boot position,
  order/timing preserved). No mod.toml = load everything (backward-compat, proven: `mod status`="loaded: items
  bundle worlds", the persisted vworld restored, alive); a section `enabled=false` is skipped; `mod reload`
  re-applies `[style]` only. RPC `mod status|reload`. **NEXT** (`mod_manifest_system_plan.md`): (3) the `[[object]]`
  proc-mesh realizer (needs the proc-mesh lib — 3D-backend step 3); (4) named style presets; (5) `mfg_min` version
  gate + realizer-logs-gaps. The framework's mod-authoring backbone.
- **★ Greybox job #2b (restyle ER's render) — PROTOTYPED + LIVE-VERIFIED 2026-07-05 (`55afc49`).** The CHEAP
  path to restyling ER's own render (vs the hard #2c shader/PSO-override the user was weighing): a full-screen
  post-process pass in the present hook — copy backbuffer → temp SRV → full-screen style shader → backbuffer,
  BEFORE our r3d+ImGui so the overlay stays crisp. `goblin_postfx.{hpp,cpp}`, RPC `postfx 0|1|toggle | mode
  <1..4> | strength <f>` (1 grayscale, 2 posterize, 3 edge, 4 edge+desat), off by default. Zero ER-shader RE,
  engine-agnostic. Screenshots: grayscale + posterize visibly restyle ER's whole frame, minimap/HUD full-colour
  on top, no crash. **⇒ #2c (RE ER's shader system) is now only needed IF a post-process look proves
  insufficient** — start from #2b. (#2a debug-flag scan `windows_debug_render_flag_re_prompt.md` still the
  free-if-it-exists option.) Follow-ups: TOML-drive the mode/strength/colour; tune edge strength; maybe a
  depth-aware pass later (needs ER's depth target). Note: this restyles the WHOLE frame globally, not per-object.
- **★ ADD-AEG (pivot 2) — BLOCKED on the injection thread; RE prompt written 2026-07-05 (`a9dc674`).** Runtime
  spawn of a real ER AEG asset (engine-rendered, the "unified render" path). Pivot 2 fully RE'd + reqMgr resolves
  live, but `FUN_1406a5080` (registrar) can't be called from the mod: **present=deadlock, worker-thread=fault
  (game-thread-bound), the 2 proximity-streamer steps don't fire (gated)** — 4 attempts, all recorded in
  `windows_geom_spawn_pivot2_re_findings.md §live-4-attempts`. The **deferred-queue scaffolding is ready**
  (`goblin_geom_spawn.cpp`: `spawn_asset` queues; swap `STREAMER_STEP_RVA` once the target is known). **Windows RE
  DONE (2026-07-05, `windows_geom_spawn_thread_re_findings.md`):** the injection point = **`FUN_1406d31f0`
  (er+0x6d31f0)** — the reqMgr per-frame update (`param_1 = DAT_143d69ba8`), called every frame in-world by the
  world-geom task `FUN_140623410` (er+0x623410) on the registrar's own main-update thread. **Set
  `STREAMER_STEP_RVA = 0x6d31f0` and hook it**; drain the queue there via the native by-id helper
  `FUN_1406d4e80(state, aegId, worldPos)` (builds `AEG###_###`+block+registrar) or the raw `FUN_1406a5080`.
  Boundness = main-update CONTEXT (single-writer reqMgr RB-tree `mgr+0x318`, not TLS). Meanwhile the r3d debug
  tool covers visualisation.
  **★ THREAD WALL PASSED — hook implemented + validated LIVE 2026-07-05 (Linux/Proton).** `STREAMER_STEP_RVA =
  0x6d31f0`, `spawn_asset` queues + `hk_step` drains inside the `FUN_1406d31f0` detour. Live: `hk_step FIRED —
  main-update thread reached`, drain ran with **no deadlock / no freeze** (game continued normally). Two bugs
  fixed en route: (a) the finding's `FUN_1406d31f0` prologue AOB is NOT unique (3 matches, first = wrong fn
  er+0x3e6bb0) → hook by RVA directly, no AOB in the health table; (b) lazy install must use
  `modutils::hook_now` (immediate `MH_EnableHook`), not `hook()` (queued for the init-time `MH_ApplyQueued`
  that already ran) — else the detour never fires.
  **★ BY-ID SPAWN REQUEST ACCEPTED — LIVE 2026-07-05 (part 2).** The raw `FUN_1406a5080(reqMgr, name)` drain
  FAULTS even on the correct thread (proven: our drain + the streamer's legit registrar calls share the SAME
  TID; replaying the streamer's OWN captured names still faults) — the raw registrar isn't a valid cold entry.
  Switched the drain to the native by-id helper **`FUN_1406d4e80(state=p1step, aegId, worldPos)`** (state =
  `FUN_1406d31f0`'s param_1; aegId parsed from `AEG###_###`; worldPos = `get_player_world_pos`): the engine
  returns **`ok=true`, nonzero handle, no fault** for every name. Diagnostic RPC verbs added: `spawn_capreg`
  (capture registrar args) / `spawn_cap4e80`. **NEXT (open, last gap):** the accepted request is NOT yet a live
  rendered instance — `geom_stats` total is unchanged (16130→16130) after 9 spawns, nothing visible. The
  request→instance build-out is the streamer state machine (`FUN_1406c6050(req,4)` → proximity step
  `FUN_140699170`, flagged gated). Drive that transition (or check for an un-enumerated class / a world↔block
  `worldPos` frame mismatch) to make it render. That's what remains to fully close runtime ADD-AEG.
- **★ r3d DEBUG-RENDER TOOL — DONE + LIVE-VERIFIED 2026-07-05 (`a9dc674`).** r3d generalised from the test cube
  to **world-anchored debug boxes at arbitrary coords** (`r3d box <x> <y> <z> [size]` / `r3d clear`) via the ER
  camera — to SEE an invisible mesh / entity / loot at its real world pos (verified: 2 boxes beside+above the
  player). The debugging-rendering use the user chose (over pushing ADD-AEG); future: viz invisible entities/loot/
  should-have-a-mesh objects.
- **★ Virtual-world 3D backend — STEPS 1+2 DONE + LIVE-VERIFIED 2026-07-05 (`42039da`, `28eabb3`).** The mod-owned
  D3D12 3D backend works (`goblin_r3d.{hpp,cpp}`, RPC `r3d 0|1|toggle`, off by default): our own pipeline draws a
  greybox wireframe cube INTO ER's swapchain via the present hook + device/queue the overlay already holds.
  **Step 2 = WORLD-ANCHORED via the ER camera:** w2s exposes `get_camera` (render-local VIEW@GameRend+0xF0 + the
  per-frame rebase origin + fovy); r3d builds `Model·T(-origin)·View·ProjNegZ` (ProjNegZ reproduces w2s's conv2
  -vz pinhole exactly). The cube sits at the player's WORLD pos + stays anchored as the camera moves — **case 1
  proven, and this CONFIRMS w2s3d end-to-end** (the agent's find_origin/VIEW/conv2/fovy; a 3D cube >> the dot,
  which had been "pending one run"). **NEXT: the proc-mesh library** (primitives → CSG → generators, no mesh file)
  + the objects TOML `[[object]]` realizer (manifest slice 3) — that's the convergence of r3d + w2s3d + the
  manifest into modder-authored 3D greybox worlds.
- **★ Virtual-world 3D backend + procedural objects — DESIGN 2026-07-05 (`docs/plans/virtual_world_3d_backend_plan.md`).**
  User insight: the greybox/virtual worlds (vision #4 job #1) need NOT be pure-ImGui-2D — a **mod-owned D3D12 3D
  backend** (reusing the device/queue/Present hook we already hold) rendering **procedural** geometry wins twice:
  **real 3D** (meshes/shading/depth) + **zero heavy-asset loading** (geometry generated from tiny TOML params →
  no FLVER/MSB/streaming/VFS → **sidesteps the create-new-content RE frontier**). Custom 3D is NOT locked to the
  7 primitives: tiers = primitives → **CSG/composite** (covers ~90% of greybox = stacked boxes) → generators
  (extrude polygon / lathe / heightfield / spline-sweep) → inline verts+tris → custom named C++ generators, all
  no-mesh-file. Objects TOML schema (core + `render.backend = imgui|mesh3d`) drafted; 3D unifies collision+render
  from one `size`. **Case split:** overlay-on-real-world (case 1) needs the w2s3d matrix (blocked on the render-
  rebase origin); a **standalone dev-dimension (case 2) builds its own camera from player pos+yaw → NO w2s3d →
  off the critical path.** Have: `add_collision` + D3D12 hook + registry + player pos/yaw + warp. Need: the 3D
  render backend + proc-mesh lib + objects TOML/realizer.
- **ER Linux/Proton fps deficit + ER frame-bottleneck profiling — INVESTIGATED 2026-07-05, no fix found.**
  Symptom: same desktop (RTX 3060 + i5-10400F, Wayland/KDE) does **60fps on Windows 10** but **30-45fps + stutter
  (dips to 22) on Linux/Proton, even VANILLA in the prologue**. Full investigation in
  `docs/re/er_frame_bottleneck_profiling_re_idea.md` (§Results). **Conclusion: distributed inherent Proton
  mono-thread/syscall overhead on ER — no single fixable cause.** `perf`→RVA proved ER is mono-thread bound but
  the profile is **FLAT** (top fn 3.67%, long tail — the "69% [JIT]" spread over hundreds of fns; hot RVAs
  `0x141C05F87`~5.5% / `0x14251BEE7` recorded for a future Ghidra-naming, engine-curiosity only). **Ruled out:**
  constant shader/pipeline compilation (PROTON_LOG: pipeline only at load, foz cache active), sync (ntsync active),
  compositor (gamescope `-f` no help), Proton version (already GE-Proton10-34), CPU governor/boost
  (powerprofilesctl perf no help), GPU/modeset/clocks (all fine), and **SetCPUAffinity.dll** (Nexus 2859 ER Stutter
  Fix — installed as a me3 native, verified pinning ~8 threads to cores 1-6 under Proton, **no help**). **Only
  remaining config lever = kernel `mitigations=off`** (perf showed `clear_bhb_loop` Spectre-mitigation + wine
  syscall dispatchers + `rwsem_spin_on_owner` lock contention taxing the syscall-heavy main thread) — user to try
  (reboot, security tradeoff). Scripts left on the game install: `Launch ERR - gamescope TEST.sh`,
  `Launch ERR - TEST (…MangoHud).sh` (enriched profiling config; stale laptop-Optimus PRIME vars dropped).
- **Sidecar save BACKUPS (defense-in-depth) — followup, not started (2026-07-05).** The strip/reinject
  bracket writes into live inventory right before ER serializes, and ER re-checksums the save itself — so
  a wrong-layout write that somehow got past `verify_inventory_layout()` would be saved as a VALID file
  (no game-side detection). The canary + fail-loud design covers the known failure modes; a backup covers
  the unknown ones for ~zero cost. Design: on the FIRST mod-write save bracket of a session (strip active,
  i.e. sidecar enabled + custom items registered), copy `g_save_path` (`ER0000.sl2`/`.err`) + `g_mfg_path`
  (`.mfg`) to a rolling `<save>.mfg-backups/` (keep N=3, timestamped) BEFORE `g_orig_ser` runs; skip if
  the bracket is inactive (zero overhead for non-sidecar users). Both paths already live in
  `goblin_sidecar.cpp` (`g_save_path`/`g_mfg_path`); the copy must be pre-serialize (the pre-strip file
  still holds the last-known-good state). NB ER already keeps its own `.sl2.bak`, but the game overwrites
  it on the NEXT save — the mod backup must survive multiple save cycles.
- **NEW map entry for MFG markers — SPIKED 2026-07-04 (`docs/re/worldmap_new_page_spike_findings.md`).**
  User goal: a new map ENTRY so MapForGoblins markers show on a custom "dev world". **Spike verdict (this
  CORRECTS the earlier "lighter data-layer" guess):** a NATIVE new page is its OWN unsolved WRITE frontier,
  NOT a light param task — the page SET is a converter array built live in `CS::WorldMapViewModel` (ctor
  `FUN_1408855b0`, count `WorldMapViewModel+0x280`=8, page-byte table `DAT_142ad82f8=[00 01 0a]`) from
  regulation; no injection path is RE'd, plus menu-tab registration + a tile-art sheet in the ERR `.gfx` are
  all write-unknown (adjacent to the MSB wall). The whole READ side is solved (projection/converter fields/
  page switch), the WRITE side is not. **⇒ Achievable path = a MOD-OWNED virtual page, NOT a native one:**
  the overlay already draws in the backbuffer and only needs the open group-id to cull (`map_renderer.hpp:4`),
  so MFG can own a synthetic group id (≥100) + a mod-defined projection (origin/scale/bias) + a mod-drawn map
  surface (bg image + pan/zoom, opened via a mod toggle) with markers tagged to it — 100% mod code, no engine
  write, Linux-doable, and it IS World Virtualization vision #1 (a custom world = a bundle whose map is this
  virtual page). Reserve native-page registration as a far-frontier item (only if the world must be a real
  in-game map TAB). **User CONFIRMED mod-owned page (2026-07-04).** Slices:
  - **✅ SLICE A DONE 2026-07-04 — the virtual-map CANVAS.** `src/overlay_panel/panel_virtual_map.cpp`: an
    ImGui window "MapForGoblins — Virtual World Map (WIP)" with a world-space canvas (drag=pan,
    wheel=zoom-about-cursor), a snapped reference grid + origin cross + grid-step legend, and a mod-defined
    world→canvas projection (`w2s`/`s2w`; cam in world units, zoom px/unit). Drawn as a sibling of the F1
    panel (compact+full). Toggle: Dev-tab button **and** `vmap 0|1|toggle` RPC (+ `overlay_api::
    virtual_map_set_open/is_open`). Live-verified on Proton (`vmap 1` → window+grid+origin render, alive).
  - **✅ SLICE B DONE 2026-07-04 — markers on the canvas.** The selected group's live markers project onto
    the canvas as colored dots (by `m.color`), with a group selector (Combo) + `Fit`-to-markers + a marker
    count readout. Live-verified: base-overworld = 6837 markers forming the recognizable Lands Between
    silhouette; `vmap group <0-3>` / `vmap fit` RPCs drive it. **On-canvas ICONS = DONE (`563a00e`, stale note
    corrected 2026-07-05):** singles draw the native state-aware glyph via `draw_marker_glyph` (reusing the native
    per-marker draw — grace discovered/undiscovered, collected-dim, cleared, rune-glow, badge) with a
    `resolve_category_icon` atlas fallback for custom-world markers and a plain circle as the mod-agnostic last
    resort; piles draw a disc + `×N`; spiderfy fans out icons. `s_show_icons` default on (F1/vmap "Icons"
    checkbox). **NOTE:** the vmap currently
    draws INSIDE `draw_panel` so it needs F1 open — slice D must decouple it (draw independent of g_show) for
    the M-open path.
  - **✅ SLICE C1 DONE 2026-07-04 — the WORLD model + registry.** `src/goblin_virtual_world.{hpp,cpp}`:
    a mutex-guarded registry of custom worlds (`{id, name, originX/Z, scale, markers[]}`) + an active-world
    id (0 = Base ER). The vmap gained a **World selector** (Base ER → live ER markers by group; a custom
    world → its OWN markers in its own coordinate namespace) — collision-free by construction (each world =
    its own coord space, framework owns ids/placement). RPC `vworld create|marker|active|list|clear`.
    Live-verified: created DevWorld, added 11 markers (a 3×3 grid + diagonals), `vworld active 1` → the vmap
    drew exactly those 11 in the world's own space (centre 1000,1000), switchable back to Base ER.
  - **✅ SLICE C3 DONE 2026-07-04 — bundle persistence.** `goblin_virtual_world` now saves/loads the
    registry (+ active id) as `<mod>/virtual_worlds.toml`; boot-loaded via `dllmain init_virtual_worlds`
    (`vworld::load_boot`) right after `world_bundle`. RPC `vworld save|load`. **E2E-verified across TWO cold
    boots:** boot-1 create "Persisted" + 5 markers + `vworld save` → boot-2 boot-load restored it (active=1,
    5 markers rendered from disk). **⚠ Required the TOML fix (`#define TOML_EXCEPTIONS 0` + `parse_file`) —
    the exceptions-ON `parse(string)` path returned an EMPTY table (0 worlds) under Proton; see the corrected
    `docs/memory/tooling/toml-parse-file-proton-bug.md`. This also flags `goblin_world_bundle` as latently
    broken (same disproven pattern) — migrate it + add a reboot test (new open item below).**
    **Remaining C:** C2 = tag EXISTING mod markers to a world (synthetic group ≥100 in `marker_layer.hpp`).
    Per-world origin/scale projection is wired (fields) but identity so far.
  - **✅ SLICE D DONE 2026-07-04 — decoupled from F1 + opens with the game MAP KEY.** The vmap now draws on
    its OWN per-frame entry (`draw_virtual_map_entry` + a `call_draw_virtual_map` loader trampoline / the
    `MFG_DrawVirtualMap` hotreload export), called UNCONDITIONALLY in the present loop — so it appears
    WITHOUT the F1 panel. **LIVE-VERIFIED: `vmap 1` with F1 CLOSED (`panel=0`) → the window renders
    standalone.** The M-trigger is wired in `draw_virtual_map`: on the `world_map_open()` rising edge with a
    CUSTOM world active (`vworld::active()!=0`) it opens the vmap, and closes it on map-close if IT opened
    it (a Dev-toggle vmap is left alone). **M-trigger not harness-verified** (driving the game map open needs
    the install's map keybind — impractical to guess-inject); it's simple code on the proven
    `world_map_open()` primitive, **user-verifiable by pressing M in a custom world**. **Follow-up:** the
    vmap is a movable WINDOW over the native map, not a full-screen replace — native-map suppression (draw
    the mod surface in the map's place) is a later polish.
  - **ENDGAME (total native-map replacement) — SCOPED + phase-1a format-CRACKED 2026-07-04, not built.**
    Vision: the mod map fully REPLACES ER's native worldmap (one UX, no marker clipping — the clipping win
    is FREE from a full-screen mod surface, no risky native suppression needed). Make-or-break = FAST-TRAVEL
    (warp-on-grace-click; primitive `warp` exists). **Phase-1a = load ER's real map ART onto the canvas** —
    fully scoped in `docs/plans/map_tile_loading_plan.md`: the DCX→DDS→GPU chain all EXISTS
    (`create_tex_from_dds_mem`, `dcx_decompress`, `tpf_find_texture`, the `read_item_icon_sheets` template,
    the img→resource offsets, `w2s` positioning); the ONE gap was the `71_MapTile.tpfbhd/.tpfbdt` archive —
    **format CRACKED: `BHF4` header (entry table {hash,size,offset}) + `BDF4` data, each entry = `DCX`→TPF→
    DDS (inner half already handled), so the only new code is a small BHF4 entry-table parser (SoulsFormats
    BXF4).** Two paths: RAM-harvest (resident `MENU_MapTile_*` via `force_load_file`, but SRV-256-cap +
    g_icon_repo-needs-a-menu wrinkle) vs disk-extract (in-game dvdbnd read — `dvdbnd_reader` is Windows-only;
    OFFLINE format-recon possible on the loose `00_Solo.tpfbhd`). **This is a real SLICE (DX12 SRV-cap
    streaming is the main engineering constraint), best as a focused fresh-context session.**
  - **Full architecture (collision / active-world / M-open / how ER's map works) = `docs/plans/
    virtual_world_multi_world_design.md`** (2026-07-04). Key decisions: the FRAMEWORK assigns position (not
    the player) — marker worlds get separate mod namespaces, walkable worlds get a reserved mapId
    (ER's dimension mechanism); active world = player mapId (walkable) or explicit bundle (marker); ER's map
    is BAKED `WorldMapTile` DDS sheets (overworld/UG/DLC = separate dimensions), so custom worlds supply
    their own image/grid. Walkable worlds also need the ADD-geom frontier (pivot 2, Windows RE).
- **✅ `goblin_world_bundle` TOML load FIXED + TESTED 2026-07-04.** Migrated to `#define TOML_EXCEPTIONS 0`
  + `toml::parse_file` (was the exceptions-ON `ifstream+parse(string)` path virtual_worlds C3 DISPROVEN).
  New `tools/rpc_tests/test_world_bundle.py` = the missing genuine save→reboot→load (boot-1 record 1 clone +
  1 set + `bundle save` → cold boot → boot-2 `bundle status` reads `clones=1 sets=1` FROM DISK): **E2E 4/4
  under Proton.** So ALL DLL TOML configs now use the only-working `TOML_EXCEPTIONS 0` path. NB the bundle
  lives in `<ERR_ROOT>/dll/offline/` (the DLL's own folder), NOT `mod/`. See
  `docs/memory/tooling/toml-parse-file-proton-bug.md`.
- **F1 category list → GRID LAYOUT (followup, not started).** The Markers-tab category list is a checkbox
  tree; with many custom worlds/categories it overflows into a long scroll. Replace with a GRID of
  icon-tiles (the category icon we just added as the tile, toggle visibility on click, checkmark/dim
  overlay for on/off), gamepad + keyboard navigable (reuse the already-enabled ImGui nav — same nav that
  drives the on-screen keyboard). Natural successor to the category-icons work (`draw_category_icon`).
  Pure ImGui slice, Linux-doable. Keep the per-category count badge + cluster toggle reachable (tooltip or
  a detail row on select).
- **In-Game World Editor (vision #2) — SLICES 1+2 DONE 2026-07-03.** F1 panel section "World Editor
  (live)" (`src/overlay_panel/panel_world_editor.cpp`): pick an AEG asset → it shows the loot item its
  MAP MARKER resolves to (live `aeg_pickup_lot`→`resolve_loot_item_textid`→`lookup_text_utf8`) → set that
  lot's `lotItemId01` to any goods id → `Refresh markers` → shows on the map. Wires the proven runtime
  primitives (`overlay_api::param_set_field` new bridge + `rebuild_markers`) to widgets. Slice-1 visually
  verified ("Lot 997230 → Bloodrose").
  **Slice 2 (2026-07-03): repoint-to-another-lot.** Panel now also sets the asset's `pickUpItemLotParamId`
  to a different EXISTING lot (non-destructive — leaves the shared lot alone), with a live preview of the
  target lot's slot-1 item before commit. Pure ImGui over already-proven bridges (`param_set_field` on
  `pickUpItemLotParamId` is the same write the RPC repoint used; live re-read via `aeg_pickup_lot` from
  slice 1). **✅ DEPLOYED + E2E-VERIFIED 2026-07-03 (Linux/Proton, 8/8)** —
  `tools/rpc_tests/test_world_editor_slice2.py` cold-boots ER, loads a save, and proves the exact panel
  write path: `help` returns the verb list; repoint asset 99030 (lot 900002000 → 997230) then
  `param_getf`==997230 AND `loot_at` resolves lot 997230 + 'Bloodrose' (textid 500020723); restore to
  900002000. Also proved the new `help`/`?` RPC verb. (Test caveat baked in: discover pickup assets by
  loot TEXTID, not name — many valid lots resolve an empty FMG name off this chain.)
  **Slice 3 (2026-07-03): per-slot re-skin — DONE + E2E-VERIFIED (12/12).** The in-place re-skin now
  targets any of a lot's 8 slots via a `Slot` selector, showing the selected slot's live item id. Added
  `ItemLotParam_map.lotItemId02..08` to the paramedit registry (offset-only, core-stable, `+0x00+(N-1)*4`)
  + an `overlay_api::param_get_field` name-addressed read bridge. `tools/rpc_tests/test_world_editor.py`
  (renamed from `_slice2`) round-trips lotItemId02 (0→424242→0) on top of the slice-2 checks.
  **Slice 5 (2026-07-03): CLONE a lot — DONE + E2E-VERIFIED (16/16).** A `Clone this lot` button
  (`overlay_api::param_clone` new bridge → `param_clone_row`) copies the current lot to a fresh row and
  pre-fills the repoint target; combined with the `refresh_markers` v2 LotReader reset (see the live
  marker regen item below), a cloned lot now resolves on the map. Test proves invisible-before /
  resolves-after refresh.
  **Slice 6 (2026-07-03): asset/item PICKER — DONE + E2E-VERIFIED (18/18).** New host module
  `goblin_world_editor.{hpp,cpp}` scans the live params into browsable lists — pickup assets
  (AssetEnvironmentGeometryParam rows with a real `pickUpItemLotParamId`, name via the loot chain) and
  named goods (EquipParamGoods, name via `lookup_text_utf8(id+500000000)`). Exposed through
  `overlay_api::we_scan`/`we_copy_assets`/`we_copy_goods` (+ POD `WEAsset`/`WEGoods`) and a `we_scan`
  RPC. The F1 `Browse (pick asset / item)` section (Scan button → client-side filter → click sets the
  Asset / New-goods-id fields). Live scan on a loaded save: 324 pickup assets, 5499 named goods.
  **Slice 7 (2026-07-03): SAVE edits as a world bundle — DONE + E2E-VERIFIED (24/24).** New host module
  `goblin_world_bundle.{hpp,cpp}` records the editor's edits (dedup: SET keeps last per param/row/field,
  CLONE unique per newId) and persists them as `<mod>/world_bundle.toml` (`[[clone]]` + `[[set]]` arrays,
  toml++). `apply_current()` re-runs them (clones first, then sets, then `reset_lot_reader`);
  `apply_boot()` re-applies the default bundle at startup (wired in `dllmain` right after
  `custom_items::apply`, before the first marker build → no LotReader reset needed at boot). Panel:
  `Save / Apply / Clear bundle` buttons + op count; RPC `bundle status|clone|set|save|load|apply|clear`.
  E2E: record clone+repoint → save → clear memory → apply-from-disk → asset resolves the cloned lot's
  item. **This is the first brick of vision #1 World Virtualization** (a swappable world = a bundle);
  remaining for #1: multiple named bundles + live swap (reset-to-base + apply + refresh) + per-world
  sidecar save context. **⚠️ GOTCHA found:** `toml::parse_file` returns an EMPTY table under Proton/Wine
  (silent, no throw) — but ONLY in the exceptions-ON config; read via `std::ifstream` +
  `toml::parse(string)` instead (world_bundle does). **custom_items.cpp CHECKED — NOT affected**: it
  `#define TOML_EXCEPTIONS 0` so its parse path works (re-verified `test_author_items.py` 1/1). No fix
  needed there. See `docs/memory/tooling/toml-parse-file-proton-bug.md`.
  **Next slices:** category select (weapons/armour/… beyond goods in the picker), and the World
  Virtualization multi-bundle swap. (`refresh_markers` v2 fully done.)

- **MSB WRITE — frontier #1, first probe scoped 2026-07-03 (`docs/re/windows_msb_placement_write_re_prompt.md`).**
  The keystone for "create new content" (custom mob/treasure placement, new map geometry). We READ MSB fully
  (both routes, `msbe::parse_msb`) but have NO write path. The probe is a cheap decision: RE the MSB→instance
  load path (static Ghidra — `CSMsbPartsGeom`/`CSMsbPartsMap` ctor) to learn whether the position is
  **snapshotted into the spawned instance** (⇒ resident-MSB writes are inert, the movable transform is on the
  instance — likely reusing the enemy/boss + FieldIns transform we already RE'd) or a live pointer; then a
  2-target live write test (instance transform vs resident MSB bytes) picks the layer. If "move an existing
  placement live" falls out, it's an immediate World-Editor slice (drag-a-placement). "Add new" then splits
  into a spawn-factory + tile re-stream follow-up. **Volet A DONE (static, 2026-07-03,
  `..._findings.md`):** the MSB position is snapshotted TWICE (`CSMsbParts` ctor `er+0xcee430` copies
  `+0x20` out of the blob → `CSWorldGeomIns` ctor `er+0x6c5900` builds its OWN transform at `+0x18` from a
  separately-passed world matrix) → **resident-MSB byte writes are provably inert.** The movable transform
  is the FD4 location module at `CSWorldGeomIns+0x18` (world matrix cached at `+0x44`), so moving needs the
  **setter**, not a flat poke. **`CSWorldGeomDynamicIns` (`FUN_1406b9880`, on factory `FUN_1406c5900`) is a
  movable geom class** = the vehicle for both move + add. **Setter FOUND (vtable-walk):** the world transform
  is set via **`vtable[0xd0]` (slot 26) `SetWorldMatrix(inst, mat4x4)`** on the FieldIns/geom instance
  (proven by two callers `er+0x6c9aa0`/`er+0x6e4210`; getter `FUN_1406c46e0(inst+0x18,&out)`, row-major 4x4,
  translation in last row). **NEXT:** the live move probe is now a direct vcall
  `(*(void(**)(void*,const float*))((*(void***)inst)[26]))(inst, mat)` behind a dev RPC — get `inst` from the
  geom manager (`DAT_143d7b0c0[+0x10]`) or the FieldIns registry; observe the object move. Then "add" = drive
  the Dynamic factory (`er+0x6b9880`) from a synthesized parts rec + transform (trace `er+0x6a7930`/`er+0x6adc80`).

- **Long-horizon vision bets — tracked in `docs/runtime_modding_framework_vision.md` "Future directions"
  (2026-07-03):** (1) World Virtualization — a FRAMEWORK feature: the framework holds N of its OWN worlds (each a data
  BUNDLE of param overrides + custom items + names + map/loot edits + flags + save context) and swaps the
  active one live over one shared base. NOT third-party-overhaul interop (the Convergence⟷ERR line is
  only an analogy). Missing = a bundle format + activation (reset-to-base + apply + `refresh_markers`) +
  per-world sidecar save context. Shares the primitives with #2,
  (2) In-Game World Editor (ImGui over the runtime primitives — the live-edit loop already EXISTS:
  `param_setf`/`param_clone`/`loot_at`/repoint/`lotItemId01`/`refresh_markers`; the editor is the panel
  wiring), (3) 3D model variants + reuse across worlds (asset/MSB frontier — needs an MSB-write path that
  doesn't exist; hardest/furthest). Not scoped; captured so they aren't lost.

- **Geom placement MOVE — CRACKED + LIVE-VERIFIED 2026-07-03 (`move_asset` RPC, 7/7).** The MSB-write
  frontier's "move an existing placement" half is solved: the transform setter is `vtable[0xd0]
  SetWorldMatrix(self, mat4x4)` on `CSWorldGeomIns`. `goblin_geom_move.cpp` picks a live geom instance
  (`goblin::collected::first_live_geom_instance()`, the WGM/CSWorldGeomMan walk — the FieldIns registry
  `[er+0x3d7b0c0]…` was EMPTY in-world) and vcalls it; `move_asset 0 100 0` moved the cached world matrix
  (`inst+0x220`, one float) by exactly +100 Y, game alive. Full RE + nuances in
  `docs/re/windows_msb_placement_write_re_findings.md` (setter writes the `+0x220` CACHE, not the `+0x18`
  module → verify via `+0x220`, not the getter). **Persistence CONFIRMED 2026-07-03** (`move_hold`/
  `move_read` RPCs): a cache-only write is DURABLE — `move_hold 0 100 0` then polling `+0x220` 10× over
  ~7s held at Y 83.38, no engine revert from the `+0x18` module. So no module-sync needed for a move to
  stick. **On-screen render CONFIRMED 2026-07-03 (user-observed live):** `move_all <d>` (mass move of
  every loaded geom instance; new `move_near`/`move_all`/`move_restore` + `list_live_geom_instances`) was
  watched live — props visibly moved. Automated screenshot pairs were defeated by First Step's scripted
  intro camera (pans between shots even after a 15s settle), so the confirm is the live observation + the
  byte-exact/persistent proof. **Move primitive is fully proven (static→live→durable→renders); it does not
  need `+0x18`/Dynamic.** **Remaining MSB-write hole: ADD a NEW placement — now SCOPED (static, 2026-07-03).**
  Traced the spawn drivers `FUN_1406a7930`/`FUN_1406adc80`: there is **NO isolated "spawn one geom" call** —
  they are the tile-streaming state machine over the loaded MSB resource, and instances are
  **placement-new'd into fixed-capacity BlockData pools** (static `+0x2b0`/stride 0x440, dynamic `+0x2c0`/
  stride 0x5b0, counts `+0x498`/`+0x49c`), then pushed into the BlockData geom_ins vector `+0x288`. Three
  add-routes ranked in the findings; **recommended first probe = route 1 `spawn_clone`**: allocate 0x5b0,
  `FUN_1406b9880(mem, srcType, CLONED existing parts rec, transform)` (reuse a resident asset so its model
  is loaded), copy a live sibling's `+0x220` matrix + offset it, push into `+0x288`; the ctor self-registers
  WGM/render/physics. Open sub-Qs (a live probe answers): does a cloned `CSMsbPartsGeom` satisfy the ctor's
  reads; is the `+0x288` push enough for render+collision or is a pool index assumed elsewhere. This is a
  multi-step build, NOT a quick primitive like move.
  **⇒ RESUME (next session): standalone-ctor ADD is a DEAD END (builder decompiled, 726f6189). Pivot to a
  streaming-path spawn. `docs/re/windows_geom_spawn_builder_re_findings.md`.**
  The pose-descriptor builder `thunk_FUN_144cbdae7` is MSVC-EH-wrapped + welded to the tile-streaming
  context; calling it standalone FROZE the game (live-confirmed). `arg4=0` is exactly what the working driver
  passes → the hang is CONTEXTUAL, not an arg bug — decomp (726f6189) confirmed there's no arg fix and no
  cheap independent `param_4` (alias guts the source, copy double-frees the owned sub-objects). So
  hand-driving the Dynamic ctor from a standalone RPC does NOT work. **`spawn_clone` was NEUTRALIZED** — it
  now only does the safe arg recon and returns the DEAD-END string (never calls the builder/ctor; no longer a
  footgun). **Real ADD = pick a pivot (from the findings):** (1) spawn on the streaming thread — hook the
  tile-stream driver `FUN_1406a7930` and inject one extra part into its per-part loop (heaviest, correct);
  (2) the asset-request path `FUN_1406a5080`→`FUN_1406c7000` — **ASSESSED 2026-07-04
  (`docs/re/windows_geom_spawn_pivot2_re_findings.md`): this is THE viable ADD route.** `FUN_1406a5080` is a
  non-blocking asset-request REGISTRAR (name/id → RB-tree at `reqMgr+0x318`, state 4, streamer services it on
  ITS thread), and consumer `FUN_140699670` proves the path yields a tracked, player-positioned placement
  from a NAME. Beats pivot 1: no standalone hang, the engine builds the owned descriptor itself, name-driven.
  **Q1/Q3/Q4 DONE (2026-07-04):** reqMgr singleton = **`[DAT_143d69ba8+0x30]`** (er+0x3d69ba8, FD4Singleton);
  name format = **`"AEG%03u_%03u"`** (request an asset by its AEG###_### name); owning feature = a periodic
  player-proximity raycast AEG-asset STREAMER (`FUN_140699670`/`d80`, steps `FUN_140699170`/`FUN_14069a550`) —
  a ready template proving "name + world pos → streamer spawns+tracks the asset." **Nuance:** the path streams
  from a KNOWN-asset registry → ideal for placing copies of EXISTING AEG assets (the world-editor case);
  arbitrary-new needs tree registration. **Q2 DONE (2026-07-04): pivot 2 STATIC RE is COMPLETE.** The `req`
  object has the EXACT `CSWorldGeom` instance layout, and `FUN_1406c6050(req,4)` is its per-frame
  load/visibility state machine: `FUN_1406c8750`→`FUN_1406a6630` (same block instance-registry the ctor uses)
  + `FUN_1406e38c0` (scene/render node + world matrix `FUN_1409f1320`). So `FUN_1406c7000` allocates a REAL
  geom instance (reconciles the earlier "just a name builder") and a serviced request → a real,
  block-registered, rendered instance — NOT visual-only. Collision follows the standard world-geom path (one
  live checkmark left). **reqMgr singleton AOB FOUND (2026-07-04, pyghidra byte-scan) — pivot-2 STATIC RE is
  100% DONE.** `GEOM_REQ_MGR = "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 16 B8 01 00 00 00 48 8D 53"` (er+0x1dcc53,
  UNIQUE image-wide; `relative_offsets {{3,7}}` → `&DAT_143d69ba8`; `reqMgr = *(singleton+0x30)`; backup AOB
  in the findings). **✅ LIVE PROBE WIRED + RUN 2026-07-04 (`goblin_geom_spawn.{hpp,cpp}` + `spawn_asset`
  RPC):** GEOM_REQ_MGR + backup AOBs `[SIG]` PASS + UNIQUE (er+0x1dcc53 / er+0x1dc930, match commit); reqMgr
  chain resolves in-world (`reqMgr=*(singleton+0x30)`). **BUT the direct `FUN_1406a5080` call DEADLOCKS** —
  our RPC runs on the PRESENT thread (pump()) yet the call froze present (watchdog, no exception; workers
  alive) = lock inversion vs the streamer on the reqMgr RB-tree (`mgr+0x318`). So the static "no hang" claim
  fails for an off-native-thread call. RPC is now safe-by-default (resolve-only; `force` to fire → hangs).
  **⇒ NEXT = call it on the game's MAIN-UPDATE thread** where the proximity streamer (`FUN_140699670`) calls
  it safely: hook a per-frame main-thread step (`FUN_140699170`/`FUN_14069a550`) and inject the request there
  (the heaviest-but-correct pivot-1 shape), NOT a standalone RPC/present-thread call. Full result in
  `docs/re/windows_geom_spawn_pivot2_re_findings.md` item 2. MOVE stays fully solved.
  **Live recon (2026-07-03, `spawn_probe` + `test_spawn_probe.py`, fresh DLL) confirmed srcType + corrected
  the layout:** on a real dynamic instance (`AEG004_903`) — srcType@+0x08 `0x3c1412016ff00000` (geom tag ✓,
  hi==BlockData tag ✓; masks g0=0xff/g1=0x14/g2=0xfffff); **param_3 = the BlockData** (inst+0x10, NOT a
  cloned record — so `rec+0x18b` = BlockData+0x18b = 0, registry on BlockData `+0xe8/+0xf8` cap 1024 room);
  **the transform module is at inst+0x20** (heap ptr), not +0x18; CSMsbPartsGeom sub-object embedded at
  inst+0x30 (vt er+0x2ba6738); the **`+0x288` geom_ins vector is EXACTLY FULL** (n=41). ⇒ spawn_clone passes
  the source BlockData, self-allocs 0x5b0, and can **SKIP the +0x288 push** for a first render probe (ctor
  self-registers into WGM/render; untracked-for-unload leak is fine for a throwaway).
  **Move-init risk SOLVED (9081c7c8, Windows-Ghidra):** `param_4` is a **~0x188-byte pose DESCRIPTOR**
  (not a 24B handle); `FUN_1406c3180` field-swaps ~0x188 bytes AND move-constructs the embedded
  CSMsbPartsGeom at self+0x30 from param_4+0x18 (`FUN_140cef4a0`) + steals the heap pose ptr param_4[1] into
  self+0x20 — so a source-aliased param_4 WOULD gut the source (and a deep-dup is a trap: pointer-rich
  sub-object). **FIX = don't alias, REBUILD:** call the driver's own builder
  `thunk_FUN_144cbdae7(&out, BlockData, partsList=*(BlockData+8+0x48), *(BlockData+8+0x58))` (er+0x6c3910,
  4-arg sig identical at both driver sites) → a fresh OWNED descriptor; the ctor move-inits from OUR copy,
  source untouched. **⚠ SUPERSEDED (726f6189):** this "rebuild `param_4` via `thunk_FUN_144cbdae7`" fix is
  DEAD — that builder is MSVC-EH-wrapped + streaming-welded and **HANGS** called standalone (it's not a
  transform math fn; it does resource/streaming work on the wrong thread). So the whole route-b/standalone-ctor
  recipe below is abandoned; kept only as resolved static facts. Real ADD = pivot 2 (above).
  Blocker 1 (srcTypeDesc) = 8-byte packed FieldIns id, buildable (live cross-check ✓: geom_dump inst+0x08
  has the `0x6…` tag + block-tag high32). Blocker 2 (transform=24B FD4 pose wrapper) SIDESTEPPED via **route
  (b): spawn at the source transform, then `SetWorldMatrix`-move to the offset (reuse the proven move
  primitive).** **`FUN_1406c7000` CHECKED + downgraded (8a0e37a) — NO shortcut:** asset-name/streaming-REQUEST
  builder, not a leaner instance factory → drive the Dynamic ctor `FUN_1406b9880` directly.
  **Blocker 3 SOLVED (4e405a1):** the ctors read ONLY `rec+0x18b` (a char flag) from the parts record — the
  prompt's guessed `rec+0x124/+0x3b/+0x3c/+0xd` are the TRANSFORM module (`param_1[4]`/self+0x20), NOT record
  fields; no model-ref read. The Dynamic ctor also MUTATES the record — `FUN_1406a6630(record, inst)`
  registers the clone into the source record's instance list (`rec+0xe8` slots/`+0xf8` cursor/`+0xfc` cap,
  guarded). Route (b) reusing the source record is field-safe (only `+0x18b` matters); the clone just becomes
  tracked by the source record's lifecycle (fine for a dev probe; production would synth a minimal record).
  **(The above blocker-1/2/3 facts are TRUE but pertain to the abandoned route-b; the Proton `spawn_clone`
  probe was tried and is a dead end — see the ⚠ SUPERSEDED note. NEXT is pivot 2, at the top of this bullet.)**
  **Freecam** (dev tool, after ADD): recon done (`windows_freecam_re_findings.md`), **Route 2** = freeze
  ChrCam + override the render view matrix in `GameRendCameraSet` (er+0x680460). BLOCKED on Ghidra: that
  matrix offset + a `CSCameraImp` singleton AOB. Then I code freeze+override from Linux.
  **Disk 90GB scare (2026-07-03) = NOT our code — closed.** User lost 90GB while ER ran, recovered on close.
  Verified: our logs 27M, no ER core dump, `move_all` (16927 insts) had zero disk effect, disk stable in
  automated tests. The Discord "16GiB deleted" files are `memfd:*_pool_shadow` (Chromium PartitionAlloc, **0
  real disk** — lsof shows logical size only). Leading suspect = **Discord Clips recording ER** (journal
  confirms Clips sessions per ER launch) filling a disk buffer during REAL gameplay; user disabling Clips is
  the fix. To re-confirm if it recurs: watch deleted-open files by REAL `st_blocks` (not logical size), while
  actually playing. Don't re-investigate our code.
- **Terrain / Havok collision WRITE — NEW frontier, first probe scoped + partly RE'd (static, 2026-07-04,
  `docs/re/windows_terrain_heightfield_write_re_{prompt,findings}.md`).** The write counterpart to the
  read-only heightfield raycast: change collision GEOMETRY (deform ground / add a platform). **Static done:**
  RTTI inventory of the full `hknp` shape+body toolkit; terrain is a **baked `hknpCompressedMeshShape`**
  (vtable er+0x2eeb908) ⇒ **Route A (deform-in-place) is a near-certain DEAD END** (an editable
  `hknpHeightFieldShape` er+0x2ee2a18 also exists — the live shape-vtable read decides). **Route B (add a
  dynamic collision body) is the path** and reuses the geom-spawn shape-of-work. `CSPhysWorld` ctor
  `FUN_140c6f120` mapped: **`hknpWorld*` @ `CSPhysWorld+0x08`** (confirms the raycast `ctx+8`), hknpWorld ctor
  `FUN_1418a6760`, world event slots `FUN_1418ae7d0`, shape-tag codec `hknpUFMShapeTagCodec<3,5,8>`
  (`FUN_14187dc50`). Route B shapes = `hknpBoxShape`/`hknpSphereShape`; vehicle = `CSPhysIns@CS` (~0x60B) +
  DLRF factory. **✅ ROUTE D LOOP PROVEN LIVE 2026-07-05** (`goblin_add_collision.{hpp,cpp}`, staged
  `add_collision` RPC, `test_add_collision.py` 9/9): cinfo(defaults + shape@+0x00 + pos@+0x30, STATIC) →
  `allocateBody FUN_1418aabf0` → `addBody FUN_1418a9ff0(mgr,&id,1,0,0)` ran from the PRESENT thread (no
  deadlock, no hook needed), and the `hf_probe_present` oracle then hit at EXACTLY the injected body's Y
  (21.44→133.37 = player+40, Δfoot=40.00), persistent ≥8s. Full result:
  `docs/re/hknpworld_addbody_slot_re_findings.md` §7. All 4 anchors AOB-hardened same day (PHYSWORLD_SLOT
  via live FWA + CINFO_INIT/ALLOCATE_BODY/ADD_BODY prologues via hf_hook_scout; [SIG] 48/48 clean).
  **Remaining:** (a) the REAL `hknpBoxShape` build — `FUN_141916c30(self, aabb[8], convexRadius, cfg)` +
  the small BuildCfg map (the probe BORROWS a live body's shape, so half-extents aren't honored yet);
  (b) walk-on-it human confirm + tile-re-stream persistence; (c) hit-normal readback was (0,0,0) on the
  injected body — benign for the probe, check when the real box lands.

- **Dev "creative mode" mini-track — SCOPED 2026-07-03 (do after ADD; not on ADD's critical path).** Two
  small/moderate RE items that together give a dev sandbox loop (warp into a throwaway map + fly around +
  place/move/verify) so world-editing experiments (move/ADD/spawn) run ISOLATED from the live save's world:
  - **(1) Warp-to-(mapid, x,y,z) dev primitive.** Today `warp <graceId>` uses `LuaWarp_01` — warps to an
    existing GRACE/bonfire by id (gated on a grace at the target). Missing = a coordinate/map-id warp (ER's
    `WarpToMapPoint`-style debug warp) so the dev can drop into ANY existing map at any coord. Moderate RE
    on top of the warp infra (`LUA_WARP` sig, `CSLuaEventManager`) + player-pos/mapid slots already RE'd.
    Sandbox target = an existing SPARSE/empty map (arena/coliseum, a cleared legacy dungeon, or an overworld
    tile) — do NOT need to create a new map (that's the capstone below).
  - **(2) Freecam.** Not tracked anywhere yet (new). Detach the camera from the player + drive it via input
    — the natural verification+authoring tool for move/ADD + the World Editor (the move/ADD confirms all
    need eyeballing the world at an arbitrary position). Moderate, well-trodden ER RE (FD4/debug camera
    struct + detach flag + input), NOT a frontier. Prompt to write: `docs/re/windows_freecam_re_prompt.md`.
  - Combined payoff: warp (1) + freecam (2) = a "creative mode" dev loop; each is cheap alone, big together.
  - **NOT this track:** creating a genuinely NEW empty map/page FROM SCRATCH = full map creation (new MSB +
    collision + streaming/worldmap registration) — strictly harder than ADD, the map-content **capstone**.
    Explicitly LATER; the sandbox uses an existing map instead so it isn't blocked on this.

- **Live marker regeneration (real-time map editing) — v1 DONE 2026-07-03; v2 open.** Markers build once
  at boot; to reflect a LIVE param edit on the DRAWN map without a game reload, **`refresh_markers` RPC**
  (→ `overlay_api::rebuild_markers` → `worldmap::rebuild_markers`, the production toggle-rebuild path) now
  forces a fresh bucket build. Verified: after a `pickUpItemLotParamId` repoint, `refresh_markers` ran a
  full `build.buckets` (2381 ms) on the detached disk WORKER thread (no frame freeze), re-reading live
  params; game alive. Since the rebuild uses the same live resolve as `loot_at`, existing-lot edits
  (repoint, `lotItemId01`, any param override) now show on the map.
  **v2 (b) DONE 2026-07-03 — cloned lots now resolve.** The 5 `LotReader` caches in
  `goblin_loot_resolve.cpp` were consolidated into ONE shared, mutex-guarded reader (callers copy the
  0x98-byte row out under the lock, then read lock-free) with a public `goblin::reset_lot_reader()`;
  `rebuild_markers()` calls it synchronously before kicking the worker, so a lot CLONED live
  (`param_clone`, which reallocates `param_header->param_table` — the pointer the reader snapshots at
  construction) is re-read on refresh. E2E-proven (`test_world_editor.py`, 16/16): a cloned lot reads
  `textid=-1` (invisible) BEFORE refresh and resolves its item AFTER.
  **v2 (a) DONE 2026-07-03 — parse cached, refresh ~60% faster.** Measured: `build.buckets` was
  ~3160ms of which the MSB parse (`load_disk_treasures`, ~480k asset placements) is ~1820ms. That parse
  output doesn't change on a PARAM edit (only the live per-marker resolve does), so it's now cached in a
  file-scope `ParsedDisk` (keyed by the source "want" flags; MSB files are immutable for the process, so
  the key is the only invalidation). A param-only `refresh_markers` reuses it (`[BENCH] build.disk_parse:
  CACHED`) → **build.buckets 3163 ms → 1262 ms**. The two vectors the build augments in place
  (`disk_collectibles` LOD-feature append, `disk_enemies` LOD-award append) get a cheap working copy;
  the rest are read-only refs into the cache (const-checked by the compiler). New `[BENCH] build.disk_parse`
  line isolates the parse cost. E2E still 18/18 (markers unchanged). Remaining perf idea (not needed):
  truly INCREMENTAL per-bucket regen — the parse cache already removes the dominant cost. NB the copy is
  ~30MB resident; acceptable. Gate any AUTO-trigger vs the collected-graying contract + `read_wgm` spike.
- **F1 panel to edit param overrides live** — optional polish on the param-override framework (all 3
  loader slices are done/merged); more registry fields = one AOB each. Not started.
- **Gap C GRANT — grant+sidecar PROVEN 2026-07-03; NAME + author surface remain.** A CLONED custom
  goods row grants into inventory and is kept out of the vanilla `.sl2` (`test_gapc_grant.py` 4/4 +
  boot-2 clean 1/1). Two findings baked into `custom_item_end_to_end_plan.md`: (1) **grantable goods-id
  ceiling `0x7FFFFE`** — `give_item` no-ops at ≥`0x7FFFFF`, so the old reserved band `90000001` was
  never grantable; use ≤`0x7FFFFE` (the test uses `8000000`). (2) **`fmg_set` slot: base `10`
  WORKS, DLC-tier `419` FREEZES** the present thread (RPC marshals there). Inject names at slot 10;
  the 419 hang + which slot the item-name UI reads are handed to a Windows/Ghidra sweep
  (`docs/re/windows_fmg_slot_re_prompt.md`). **RESOLVED (static, 2026-07-03,
  `docs/re/windows_fmg_slot_re_findings.md`):** goods-name UI (`FUN_140d10680`) reads
  `menu(111)→base(10)→dlc01(319)→dlc02(419)`, so a NEW id renders at base **slot 10** (111/DLC empty
  for it); the 419 freeze is our `patch_fmg_in_memory` doing a `fileSize − str_data_start` size_t
  **underflow** on a DLC-stub header (NOT the group loop — hence the reverted span guard didn't help)
  → multi-GB resize/memcpy on the present thread. Fix = O(1) offset/size sanity guard + reject slots
  ≥300 and the 11x menu tier; keep injecting at base 10. **DLL guards CODED + verified 2026-07-03**
  (`goblin_messages.cpp`: `patch_fmg_in_memory` offset/size + span-vs-stringCount guards;
  `inject_fmg_entries` slot policy): `fmg_set 419` now returns a fast error (game alive), `fmg_set 10`
  works, boot PlaceName(19)/TutorialBody(208) injects unaffected.
  **✅ Author surface DONE 2026-07-03** — `custom_items.toml` (TOML chosen over JSON for hand-authoring;
  toml++ header-only). `goblin_custom_items.{hpp,cpp}` applies each `[[goods]]/[[weapon]]/…` at boot
  (clone+fields+name) + `sidecar::register_author_item` (declarative registry: granted on world-enter,
  stripped pre-save, NEVER in the `.mfg` — re-applied every boot). E2E `test_author_items.py` 1/1: toml
  → boot → world-enter grant → `goods_count==qty`. Example `custom_items.example.toml`.
  **Remaining polish only:** finalize the reserved band from a param-scan survey; `decode_textid`
  read-back chain parity (menu-first `{111,10,319,419}`); more categories as needed. **Gap C is
  functionally complete.**
- **MapGenie coverage — Hidden Passage category, not started.** Hit-detected illusory walls, no action
  button → no static signal to parse (hardest remaining Group-2 category). RE notes:
  `docs/re/windows_group2_landscape_re_findings.md`.
- **MapGenie coverage — Wandering Mausoleum, not attempted.** Dynamic moving entity, no static MSB
  signal; low priority.
- **RPC auto-idle (`feat/rpc-auto-idle`) — needs in-game verify.** Built + deployed
  (`src/input/input_wndproc.cpp`, `goblin_debug_rpc.cpp`, ini `[Debug] rpc_auto_idle` default true):
  scripted RPC input (`key`/`mouse_*`) should self-suspend for ~1.5s when the human touches real
  keyboard/mouse, and NOT self-idle from its own injected input. Verify with the map open: wiggle the
  real mouse → `status` shows `rpc_input_idle=1` within ~1.5s and an RPC `key` is refused; stop
  touching input → resumes after ~1.5s; a scripted `type`/`key` run must NOT trigger it. Dev-only
  tooling, no changelog line on pass — just merge. Detail: `docs/memory/tooling/mfg-rpc-driver-hardening.md`.
- **Silent deadlock freeze — UNSOLVED.** One occurrence (2026-07-02): log goes silent (no crash, no
  exception), window solid, RPC thread alive. Distinct from the known `eldenring.exe +0x1EB9999` exit
  crash (that one's handled: TerminateProcess after triage). Shipped the catcher —
  **freeze watchdog** (`goblin_freeze_watchdog.cpp`, ini `[Debug] freeze_watchdog_secs`, default 20s):
  present-thread heartbeat; on stall writes `logs/MapForGoblins_freeze_<pid>.txt` + a full-thread
  minidump. **Next freeze → symbolize the dump with the deployed PDB and root-cause.**
- **Background-focus RPC driving — partially closed.** Root cause found: our own `g_has_focus` gate
  kills keyboard poll + mouse clicks off-focus (not the pause system). The first `key` after
  auto-refocus being silently lost is fixed (closed-loop retry via `hk_wndproc` arrival counter). Still
  open: `mouse_click`/`type` have no delivery-verify (same loss window), and "drive UI while the user
  works elsewhere" needs a dev-mode treat-as-focused override (accepted tradeoff: RPC keystrokes leak
  into the backgrounded game, symmetric with how PauseTheGame's global hotkeys already behave). Not
  started; until then keep the game window focused during scripted UI runs.
- **F2 fog-locate pan clamp — reverted fix, real bug still open.** Locating a target in undiscovered/
  fogged territory (e.g. Morgott while Leyndell is fogged) clamps the pan at the edge of revealed area
  instead of centering the target — deterministic repro documented. A fix attempt (direct pan/snap-rect
  writes, zoom-easer write) was REVERTED by user call; **read
  `docs/re/linux_f2_fog_locate_clamp_re_findings.md` before retrying** — the real blocker is the engine
  clamping the cursor reticle inside a `c32f0` step whose bounds source isn't in any struct we've found
  (needs Ghidra on the `c32f0` subtree). Hard constraint for any retry: non-fog locates must behave
  exactly as today, no per-frame write fights, no forced zoom.
- **Baked-data → runtime/disk migration — IN PROGRESS.** Authoritative plan:
  `docs/plans/baked_data_full_removal_plan.md` (6 phases; `build_pipeline.py` deletion is Phase 5, the
  END state, not the first step — it still generates tables with no runtime source). Landed: Phase 1
  (enemy-drop labels), name-alias English search (now reads live `msg/engus` off disk), several
  category-exception bakes recovered live via `EquipParamGoods.sortId`. **Next pick (easiest→hardest
  per the plan's inventory):** dedup `goblin_tile_tabs`/`goblin_major_regions` (identical across
  profiles now that there's only one profile — pure housekeeping); assess
  `goblin_region_anchors`/`goblin_name_regions` vs `WorldMapPointParam`+`WorldMapPlaceName`; the icon
  atlas (biggest remaining item, see next bullet). Minor unblocking follow-up noted, not gating: a
  handful of Reforged item families / DLC key items still fall into the "Loot - Crafting Materials"
  catch-all on colliding sortIds — needs dedicated rules or accept the catch-all.
- **Baked-atlas removal — DEFERRED, gate not passed.** `[ICONTIER]` census (kept in-tree for
  re-auditing) shows ~15 categories still resolve only through the baked atlas on ERR and/or vanilla
  (Hostile NPC, Spirit Springs, Stakes, Cookbooks, Crystal Tears, Golden Runes Low, …); until native/
  disk resolution covers those, the atlas stays. Re-run recipe and follow-up ideas in the file this
  replaced (`git log -p -- docs/HANDOFF.md` if needed) or re-derive via the `[ICONTIER]` census tool in
  `map_renderer.cpp`.
- **Lag-spike hunt — `read_wgm` cache-miss path still spikes.** The steady-state RB-tree walk was fixed
  (bulk RPMs, `read_rb` helper) and AVG dropped to ~0.05ms, but fresh-tile loads still spike 2-3ms
  (~33x) because every new tile re-reads each geom instance's full chain (~3 RPMs/instance) before the
  AEG family filter drops the noise. Next ideas: budget cache-miss resolution per refresh (check the
  collected-graying contract first so a deferred tile doesn't flash wrong), or land an AOB-pinned
  O(1) collected getter to skip the RPM snapshot entirely (`goblin_collected.cpp:543` already has a
  DR0 armed for this). NB `present.overlay_total`/`present.newframe` spikes were investigated and
  RESOLVED AS WONTFIX — game-side frame cost and a one-time ImGui font-atlas upload, not our code.
- **Map-exit input softlock — external cause, low priority.** Root cause is Deskflow (cursor-sharing
  KVM), not this mod or ER; fix is Deskflow-side. `docs/re/windows_input_softlock_re_prompt.md`.
- **Open policy question: is non-ERR/vanilla a hard support target?** Decides whether ERR-leaning bakes
  (atlas, etc.) can eventually be dropped entirely or must stay as a permanent vanilla-compat net.
- **Double-DLL-load hardening — not implemented.** Strategic fix (single-DLL migration) landed and
  prevents NEW installs from double-loading, but an existing install with a stale `_vanilla.dll` still
  can. TODO: a named-mutex check (`CreateMutexW`) at init so a second instance bails before installing
  any hooks and shows an on-screen "double load detected" banner instead of silently double-drawing.
- **Clang-only toolchain — Phase 1 mostly done, matrix open.** `build.bat` (ninja+clang-cl) and
  `build.bat snapshot` are both validated on Windows (packaging + PDB archival proven). Still open:
  `build.bat release` (version-bump path) unexercised; Phase 2's real in-game validation matrix.
  `docs/plans/clang_only_toolchain_plan.md`.
- **Big-files refactor — items 1+2 done, 3-7 open.** `docs/plans/big_files_refactor_plan.md`: done =
  panel split into `src/overlay_panel/`, shared marker gates. Remaining: classify dedup, diag
  quarantine, `icon_uv`, god-function breakup, grace-sprite design.
- **Real map clipping (RE the game's own clip) — not started.** Would replace the exclusion-zone
  stopgap (dial disc + user rects) with pixel-perfect clipping identical to the game's own map/minimap
  clip. Big RE; low priority, current stopgap works.
- **Zoom+pan simultaneous 1-frame icon "dash"** — stale projections streak icons for a frame when zoom
  and pan change together. Suspect: the ViewDelay ring interpolating pan/zoom inconsistently. Not
  investigated.
- **Fan (spiderfy) near a screen edge can overflow off-screen** — the canvas clip trims it but doesn't
  re-anchor the fan. Minor, not investigated.

### Decided against (don't re-propose without reading why)

- **Merchant map pins (search Slice 3) — SHELVED 2026-07-03 after an RE spike.** The shop↔NPC join is
  talk-ESD-only (confirmed no EMEVD `OpenRegularShop` signal exists); pulling a shop-id range out of ESD
  needs a full EzState bytecode evaluator — disproportionate for one pin category. Merchant item
  *search* (Slice 1) is the shipped feature; naming the seller (Slice 2) was separately deferred for
  the same ESD reason. `docs/plans/merchant_item_search_plan.md` Slice 3.
- **F2 zoom-easer write fix — REVERTED.** Mechanically worked but forced an uninvited zoom on every
  fog-locate plus a visible flicker fight on clamped targets; user rejected the UX tradeoff. See the F2
  entry above for the real fix direction.

## Standing gotchas & non-obvious facts

- **RPC/driver scripting gotchas** (full detail `docs/memory/tooling/mfg-rpc-driver-hardening.md`): a
  background job can only keep ER alive via a single FOREGROUND blocking bash command (me3 as an
  in-shell child, killed before return); `ping` ≠ game alive, gate on real liveness; AZERTY layout means
  SendInput's VK→scancode uses US but the return scan→VK translation uses the HOST layout, so scripted
  `type` must send QWERTY-position characters, not the intended letters; `mouse_move` needs the
  SetCursorPos trampoline + a real ±1px jiggle event (absolute SendInput alone lands off-target and the
  game re-warps the raw-input reticle onto the old position after one frame) — send it twice, a rare
  warp race eats the first; `pkill -f "Game/eldenring.exe"` also matches the driver shell's own args and
  kills it early.
- **Wineserver RPM contention:** many small `ReadProcessMemory` calls in a hot per-frame path can
  contend with the render thread even at sub-ms each, because wineserver serializes ALL RPM calls
  process-wide under Wine — batch reads into as few RPM calls as possible (lesson from the `read_wgm`
  spike fix).
- **Double-DLL-load is not a code bug.** If both an ERR and vanilla DLL variant ever end up in the mods
  folder, both load into the same process → doubled ImGui draw, doubled PlaceName patch, etc. Single-DLL
  migration prevents this for new installs; see the hardening TODO above for stale existing installs.
- **AOB doctrine:** pin code sigs, never raw RVAs — the WorldChrMan resolver was flipped from
  RVA-first to AOB-first after an audit found it violating this (a future ER patch that moves the slot
  would otherwise silently go stale). `goblin_world_position.cpp`.
- **Grace icon scale is deliberately SEPARATE from the generic marker scale** — calibrated for vanilla
  parity when the cursor locks onto a grace; do not fold it into the shared scale knob.
- **Golden-rune glow sizing:** size any glow/backing effect off the icon's NATURAL draw size (`base_hh`,
  a ratio), not the post-bump scaled size — sizing off the scaled size produced a big dim wash instead
  of a compact bright orb.
- **The 7 mod-added POI categories** (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have no ERR-custom glyph; their massedit iconIds
  (374+) point at glyphs absent from every current menu file (numeric glyphs cap at 261) — recover via
  a real `SB_MapCursor` glyph where one visually fits, else circle.
- `MENU_MAP_ERR_*` (boss/grace) names are ERR-only; they won't resolve off-ERR → falls back to circle
  if the baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
- i18n: `overlay_language = auto` reads the WINE prefix locale under Proton (usually `en_US` even on a
  French desktop) — French users should set it explicitly. Avoid `œ` in translations (outside the
  merged font ranges, use "oe"). Keep label translations ≲ English+20% (panel caps at 840px).

## ⇒ SESSION WRAP 2026-07-06 (Linux/Opus) — two subagent scoping reports (DX + pipeline) queued for next session

Two read-only Sonnet subagents ran at session end; findings banked here (NOT yet actioned).

**★ vmap/minimap player DX (3 real bugs, all RENDER-side = hot-reloadable) — ✅ ALL FIXED 2026-07-06 (both builds link clean; NOT yet in-game verified — do a `reload_overlay` pass next boot):**
1. **✅ Player blue circle not zoom-aware** — the constant-px halo behind the player pin now fades as `s_zoom`
   climbs past 0.30 px/unit (gone by ~1.0) so it stops blobbing over the character when zoomed in
   (`panel_virtual_map.cpp`, the `MENU_MAP_Player_01` halo). ⚠ used a manual taper, NOT `std::max` — the
   windows.h `max` macro clobbers `std::max(` in this TU (compile error `expected unqualified-id`); use
   `(std::max)()` or a plain ternary here.
2. **✅ Player z-order wrong on vmap** — moved the whole player-cursor block to AFTER the custom-pins + death-
   marker blocks (still before region labels, which are text pills meant to sit on top), so the player draws
   over them now. Left a pointer comment where it used to be.
3. **✅ Minimap settings invisible while vmap open** — `panel_settings.cpp` now stamps `s_minimap_focus_frame`
   every frame its "Minimap (in-game HUD)" CollapsingHeader is open + exposes `minimap_settings_focused()`
   (frame-stamp, auto-expires ~1 frame after the settings stop drawing); `map_renderer.cpp:draw_minimap` gates
   the vmap/native-map hide on `!minimap_settings_focused()` → the minimap force-draws over the vmap while you
   tune its sliders, giving live feedback.

**★ build_pipeline.py dead-stage audit (cleanup-scoping):**
- SAFE DELETE (confirmed dead, no Stage + no consumer): `generate_{gestures,hero_tomb_statues,hostile_npcs,imp_statues,kindling_spirits_massedit,maps,material_nodes,paintings,pieces_massedit,seal_puzzles,spirit_springs,stakes,summoning_pools}.py`, `extract_seal_puzzles.py`, + NEW `generate_model_aliases.py` and its ORPHANED uncompiled output `src/generated/goblin_model_aliases.{hpp,cpp}` (not in CMakeLists). Likely also `extract_rune_positions.py` (superseded), `extract_err_collectibles.py` (write-only output).
- CACHE BUG (like boss_list): `generate_data` stage declares stale unread inputs (`items_database.json`, `npc_name_text_map.json`, `WorldMapLegacyConvParam.json`) AND omits its REAL input `data/categories.json` → editing categories.json won't invalidate the cache. Fix the inputs list.
- `generate_loot_massedit`: 3 `.MASSEDIT` outputs + `loot_lot_linkage.json` are DEAD (write-only, 0 readers); `item_icon_table.json` LIVE only for offline QA (`taxonomy_classifier`/`unplaced_items`/`_validate_taxonomy_map`). The stage comment (:247-248) + `docs/plans/baked_data_full_removal_plan.md:118-122` are STALE (claim compiled artifacts that no longer exist) — update them.
- HUMAN CALL: is the offline taxonomy-QA tooling worth keeping 5 upstream stages alive (extract_goods_categories/tutorial_codex/items/enrich_fallback + massedit)? `extract_placename_dump` only feeds manual tools now.
- INVERSE GAP (not dead — undocumented MANUAL regen): `build_{name_regions,major_regions,region_anchors,tile_tabs}.py`, `extract_map_name_regions.py`, `generate_quest_gates.py`, `extract_quest_npcs.py`, `generate_overlay_icons.py` produce COMPILED files but have NO Stage → run manually + output committed. Worth a tooling memory note.

Both are clean next-session tasks. DX fixes are quick + hot-reloadable; the pipeline cleanup is a deliberate sweep (safe-deletes + the generate_data cache fix first, then the human-call decisions).
