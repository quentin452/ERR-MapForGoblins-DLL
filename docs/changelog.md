# Changelog

All notable changes to this MapForGoblins fork are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); this fork does not yet cut
named releases, so everything fork-specific lives under **[Unreleased]** until the first tag.

## Changelog workflow

- Every completed task that **adds a feature** must add a line to **[Unreleased]**.
- Do **not** log a bug fix for a defect that was introduced *and* fixed within the current unreleased
  cycle — the fork has no prior release, so such bugs never reached a user and net to zero against
  upstream. Only log a `Fixed`/`Performance` entry when it repairs a defect present in **upstream** (or
  a future shipped release), i.e. a difference a migrating user would actually perceive. Deep technical
  post-mortems of intra-cycle churn belong in `docs/memory/`, not here.
- Group entries under the standard headings: `Added`, `Changed`, `Fixed`, `Performance`, `Removed`.
- On an official release, move the accumulated `[Unreleased]` entries under a new named version
  (e.g. `## [v1.0.0] - YYYY-MM-DD`) and leave `[Unreleased]` empty for the next cycle.
- Keep entries short and user-facing; deep technical detail belongs in `docs/memory/` and `docs/re/`.

---

## [Unreleased]

Everything below is specific to this fork (`master`, ~990 commits ahead of `upstream/main`) and
not present in the upstream ELDEN RING Reforged / MapForGoblins project.

### Added
- **9 native-pin parity landmark categories** (`World - Churches / Ruins / Rises & Towers / Shacks /
  Forts / Castles / Towns & Villages / Colosseums / Unique Sites`) — completes coverage of every pin
  family the game itself draws on the world map, from a full audit of `WorldMapPointParam` iconIds
  (2026-07-02). Same live mod-agnostic `build_live_landmarks` pass as the Group 1 landmarks (no bake);
  unlike the native pins (which appear only once discovered), these show everything. ~175 new markers
  across the 9 categories; iconId 62 (Ashen Leyndell) also joined `World - Legacy Dungeons`. All off
  by default. Deliberately not covered: boss pins (own pass), graces (own layer), structural no-text
  nav points, legacy-dungeon sub-zone labels (iconId 42), Volcano Manor request markers (dynamic).
- **Loot - Farmable Drops category** (`WorldFarmableCollectible`, MapForGoblins-original) — marks where
  you can farm notable upgrade mats: enemies that **respawn** (no persistent obtained flag) AND drop a
  **Smithing Stone / Golden Rune / Glovewort**. Surfaces the farmable enemy drops the notable-loot pass
  previously skipped; trash drops (Sliver of Meat, …) stay hidden so the map isn't flooded. Scans all 8
  lot slots (the notable item usually sits in slot 2, behind a craft material) and labels each marker
  with that item. Live, no bake; deduped per lot (~70 spots on ERR). Off by default. (The companion
  `WorldFarmableEnemy` — marking every respawning mob — was intentionally NOT added: it floods the map
  and there is no clean live boss filter to exclude fog-gated bosses.)
- **World - Portals category** (MapGenie Group 2, first non-`WorldMapPointParam` category) — Sending
  Gate / waygate locations, resolved fully at runtime with no bake. A portal is an `AEG099_510`
  sending-gate asset whose EntityID is bound as arg[2] of EMEVD warp template `90005605` — the
  mod-agnostic signal that isolates the ~23 real player-usable gates from that model's ~180
  decorative/anchor placements. Harvested live from the active install's `event/*.emevd` +
  `map/MapStudio` MSBs, so it is correct on any install. Off by default; labelled "Sending Gate".
- **World - Miquella's Cross category** — the 13 DLC Miquella's Crosses, a clean
  `WorldMapPointParam.iconId` (208), wired through the same live landmark pass. Off by default.
- **6 landmark map categories** (`World - Divine Towers`, `Evergaols`, `Minor Erdtrees`,
  `Grand Lifts`, `Dungeons`, `Legacy Dungeons`) — closes the MapGenie landmark gaps that are a clean
  `WorldMapPointParam.iconId` key. Read LIVE from the active install's `WorldMapPointParam` (same
  `build_live_bosses` path), so they are automatically correct on any mod/vanilla — no baked data.
  Each has its own `show_*` toggle in the World section (all off by default). "Dungeon" is the union
  of ER's typed minor-dungeon icons (Catacombs/Caves/Tunnels/Wells/Hero's Graves + DLC); "Legacy
  Dungeon" is the per-site set (Stormveil, Raya Lucaria, Leyndell, …). In-game confirmed on ERR
  (114 markers, positions correct). MapGenie categories that are NOT `WorldMapPointParam` (Smithing
  Table, Portal, Hidden Passage, …) are deferred to a later MSB/AEG pass. Circle fallback until each
  landmark's real `SB_MapCursor` glyph is wired (followup).
- **`[BENCH]` logging gates** — two new independent INI settings, `bench_log_individual` and
  `bench_log_session` (both default `true`, matching prior behavior). Turn either off to keep
  only the per-call timing lines or only the end-of-session summary table; turn both off to
  silence `[BENCH]` entirely. Does not affect `[BENCH][SPIKE]` lag-hitch warnings, which always
  fire regardless (anomaly alert, not routine noise).
- **On-screen keyboard for gamepad text entry** — the item search, category filter, and quest NPC
  filter fields each get a "Kbd" button opening a popup keyboard (Alphabetical or QWERTY, pick in
  settings) built from ordinary buttons, so ImGui's existing gamepad nav drives it for free. Also
  fixes a bug where the mouse could get fully locked out after opening F1 via a gamepad combo: the
  cursor-recenter feature's own `SetCursorPos` call was generating a `WM_MOUSEMOVE` that looked
  like real mouse input, re-arming itself every frame in an infinite loop while the controller was
  active. dx-bugs-backlog PR C-2 part 2 (item 3) — gamepad-only play is now fully supported.
- **Gamepad overlay toggle + cursor recentering** — a configurable XInput combo (default `Y+R3`)
  opens/closes the F1 overlay, mirroring the keyboard toggle (edge-detected, foreground-gated).
  Cursor auto-recenters to the window center on the mouse/keyboard→pad-only input transition (ER
  itself doesn't) and on the world map's (re)open transition, so ImGui's cursor and ER's native
  cursor agree. XInput is resolved dynamically (`xinput1_4`→`xinput1_3`→`xinput9_1_0`, no new link
  dependency). New in-menu "Record gamepad combo" button captures a held multi-button combo (on
  release, not on the first button pressed) and saves it to the ini immediately. dx-bugs-backlog
  PR C (items 2, 3, 6) — see `docs/plans/dx_bugs_backlog_plan.md`.
- **Full gamepad navigation inside the F1 panel** — D-pad/left-stick moves widget focus, A/B
  activate/cancel, using ImGui's own built-in gamepad-nav backend (`ImGuiConfigFlags_
  NavEnableGamepad`, one line — the vendored Win32 backend already polls XInput for this). The
  actual work: `XInputGetState` is polled, not message-based, so it can't be swallowed like mouse/
  keyboard input while F1 is open — hooked it (MinHook, same idiom as the existing `SetCursorPos`/
  `ClipCursor` hooks) so the game gets a connected-but-idle controller state while the panel has
  nav focus, while ImGui's own nav (and our own poll) still see the real state. Also fixes the
  combo recorder capturing the very button used to click it, and adds a guard against recording a
  single nav-reserved button (A/B/X/Y/D-pad) as the toggle, which would otherwise close the panel
  on every ordinary widget click. dx-bugs-backlog PR C-2 part 1 (item 3) — search-bar text entry is
  a separate, not-yet-started follow-up (PR C-2 part 2).
- **`[quest]` badge in the item search** — search results that correspond to a quest-NPC map pin now
  show a `[quest]` tag, so you can tell which hit is the quest NPC on the map without clicking each one.
- **All quest NPCs pinned (runtime, mod-agnostic)** — the map now pins EVERY quest NPC the active
  mod's EMEVD exposes (not just the 3 hand-authored ones), each resolved by a single
  `entity_world_pos(pinEntity)` lookup from the runtime extractor. The 3 authored NPCs
  (Boc/Alexander/Thops) keep their step-following pin + rich tooltip; the rest are pinned statically
  at their first placement with a localized-name tooltip. Boss/asset-placed NPCs (e.g. Blaidd) pin
  even when unnamed. A pin shows a live `[concluded]`/`[in progress]` state only when its flag is a
  hand-vetted "dead/gone" flag; unvetted runtime flags (merchants like Kalé, whose `_q99` is shared
  between death and completion) show a neutral `optional` tag instead of a misleading state.
- **Quest-NPC map glyph** — quest NPC pins now draw the game's real NPC map symbol (the framed-hood
  glyph, `MENU_MAP_80`) instead of a plain circle. Resolved mod-agnostically by iconId via the same
  native-then-disk path as the summoning-pool effigy — reads the ACTIVE install's `SB_MapCursor`, no
  baked atlas — so it is correct on any mod, and falls back to the circle if the glyph can't resolve.
  Requires the native-icons toggle on. See `docs/memory/features/quest-browser.md`.
- **Off-page altitude badge** — the ▲/▼ altitude cue now also appears on map pages the player isn't on,
  referenced to the nearest grace in the marker's own area (the player's Y is in a different frame
  there). Grace-relative badges use a distinct tint (green above / teal below) vs the warm/cool
  player-relative badge. Grace `posY` is captured live from `BonfireWarpParam`; the nearest same-area
  grace is precomputed per marker at build. See `docs/plans/offpage_altitude_via_grace_plan.md`.
- **Item stacking** — loot markers of the SAME item within ~5 m of each other (e.g. the 4 Siofra River
  Formic Rock nodes) draw as ONE marker whose tooltip shows the combined ` xN` count (depletes as you
  gather; the stack grays only when all are collected). Co-located groups are annotated once at build
  (keyed on MSB-local world position, so it works underground where render-time tile clustering can't),
  but the `stack_identical_items` toggle (F1 menu, default ON) is a pure RENDER decision — **instant,
  no bucket rebuild** (like require-fragment). Off → every node draws individually with its own count.
  See `docs/plans/item_stacking_plan.md`.
- **Loot item count** — a lot-backed loot marker now shows the deterministic item quantity in its
  hover tooltip as an ` xN` suffix (e.g. a single-slot "5× arrows" lot → `x5`). `Marker.count` reads
  it live from `ItemLotParam` (`lotItemNum01 @ +0x8A`, `lotItemBasePoint01 @ +0x40`) — any mod, no
  bake. Slots are a SINGLE WEIGHTED ROLL, not additive, so the count is a slot's num only when one
  slot is live (basePoint>0); multiple live slots = RNG → `x1`. (Several guaranteed items are sibling
  lot rows, each its own marker — not multiple slots.) See `docs/plans/loot_item_count_plan.md`.
- **Tile-based clustering** — map markers now cluster by their map-space 256-unit tile (+ map layer)
  instead of the old nearest-grace heuristic: deterministic, zoom-aware, and piles can't drift since
  each group is bounded to one tile. Graces are never piled (vanilla parity), clustering only uses
  live-projected positions (no baked scatter underground), and with plain clustering any co-located
  tile piles — the size threshold is an adaptive-only knob (distance ramp: detail near the player,
  denser far away). New `cluster_debug_markers` overlay shows each marker's projection/tile state.
- **Altitude cue** — markers above/below the player's elevation get a small ▲ (above) / ▼ (below)
  triangle, so you don't search the wrong floor/cliff. Drawn as primitives (no font dependency); only
  shown for markers on the player's current map layer (a dead-zone hides near-level ones). The MSB
  block-local Y (`pos[1]`), previously parsed-but-dropped, is now threaded onto markers. Toggle in F1
  ("Altitude arrows") or `altitude_cue` / `altitude_deadzone` in the ini.
- **Icon legibility pass** — small loot/item map icons no longer blend into the map art: they get a
  minimum on-screen size plus a dark backing disc (only when actually small). Native map symbols
  (graces, bosses, summons) are left untouched. Config: `icon_legibility` (default on) +
  `icon_min_half_px`. Also dropped the now-redundant discovered green-check on graces (the
  undiscovered-cursor vs discovered-effigy icon already encodes that state).
- **Per-item loot icons** — lot-backed loot markers now draw their OWN inventory icon instead of one
  shared category-representative icon. At marker build, the live ItemLotParam row is resolved to the
  item's real `EquipParam` iconId (`resolve_loot_item_textid` → `item_real_icon_id`) and stored on the
  marker; the renderer prefers `native_item_icon(item_icon_id)` (resident GPU → disk), falling back to
  the category rep → baked atlas → circle on any miss. Mod-agnostic (reads the active install's params).
- **Summoning Pool glyph (Martyr Effigy)** — `World - Summoning Pools` markers now draw the native
  `MENU_MAP_89` glyph. Resolved by iconId from the active install's map-point layout, with a disk
  (no-bake) fallback when the resident GPU symbol isn't loaded — mod-agnostic, not an ERR bake.
- **Map-point disk fallback (mod-agnostic)** — `MapPointProvider` now falls back to the on-disk glyph
  by iconId when the resident GPU symbol is unavailable, so any category with a map-point iconId renders
  correctly even before/without the world map loading that symbol.
- **Self-rendered map overlay** — all goblin markers drawn by an in-process ImGui/DX12 overlay
  projected onto ER's world map, replacing native `WorldMapPointParam` injection (the sole shipped map path);
  this also eliminates upstream's map-open freeze, since the engine no longer walks thousands of injected rows.
- **In-game settings overlay** — DXGI-Present-hooked ImGui UI (F1) for live per-section / per-category
  toggles, clustering, and save-to-INI; replaces the old F-key + INI-restart workflow.
- **Player-centred minimap HUD** — corner minimap reusing the overlay/atlas chain, on every map page.
  Now honors the same marker-scale settings as the worldmap and scales to the live render
  resolution (was fixed pixel sizes, unscaled at 720p/4K), has its own lightweight screen-space
  clustering (own tuning, not the worldmap's — piling by cell rather than distance-adaptive), and
  shows the same yellow ring the worldmap draws around an active item-search "locate" target.
  Zoom/radius defaults raised through live user tuning (`minimap_zoom` 0.08→2.0, slider max
  0.30→5.0; `minimap_size` 130→100).
- **Item / object search bar** — F1 search that locates a marker by name and pans+zooms the live map onto it.
- **Quest Browser** — in-overlay ordered per-NPC step list with persisted checkmarks, missable warnings,
  and grey-out of unfinishable questlines from EMEVD-derived death flags.
- **Quest-aware NPC marker layer** — 344-marker WorldQuestNPC layer with optional quest-active gating.
- **Coverage-gap detector** — opt-in SetEventFlag + AddItemFunc hooks that toast "unmapped item collected".
- **Live world→map projection** — engine-native projection reverse-engineered and wired (`liveProjection`),
  fixing hundreds of misplaced markers; live "you are here" player position on every page.
- **Native map-point icons** — Oodle-IAT-hooked DDS harvest draws the game's real grace/boss pins on markers.
- **Unicode overlay font** — embedded DejaVu Sans TTF (Latin-Ext / Greek / Cyrillic) over ImGui's Latin-1 default.
- **F1 panel settings search** — a "find setting..." box near the top of the panel filters the whole
  panel by keyword: it matches section titles AND the setting labels inside them (e.g. "opacity"
  finds Minimap), hides everything that doesn't match, auto-expands what does, and says so when
  nothing matches. Clear the box to restore the full panel.
- **World - Elevators category** — lever-lift locations built live from disk MSB ObjAct events
  (subtype 7) filtered to the AEG027_* lift family whose ObjActParam prompt is "Pull/Push lever"
  (live ActionButtonParam text join), top/bottom lever pairs folded by proximity. 54 markers,
  in-game verified. Mod-agnostic, no bake, default OFF.
- **World - Smithing Tables category** — AEG099_308 assets from the disk MSB enumeration (4
  markers). Mod-agnostic, no bake, default OFF.
- **Sections & categories rows sorted A→Z** — category rows in the F1 panel now sort
  alphabetically by label instead of enum order (new categories used to pile up unsorted at each
  section's end).
- **Symbolized crash triage** — the build emits `MapForGoblins.pdb` (`/Z7` + lld `/debug`) and the
  crash handler resolves fault + stack addresses to function names via dbghelp when the .pdb sits
  next to the DLL; `tools/resolve_crash.py` symbolizes a triage .txt offline (function + file:line)
  with llvm-symbolizer. eldenring.exe frames stay raw offsets (Ghidra path unchanged).

### Changed
- **No-bake data pipeline** — markers derived live at runtime from the active mod's MSB / EMEVD / ItemLotParam
  instead of a committed static bake; baked marker count driven from ~8419 → 0 for ERR.
- **Loot sourced from real files** — treasure (DiskMSB), AEG collectibles, enemy drops, and EMEVD passes,
  with the bake kept only as a curated residual oracle.
- **Item classification via ER's own taxonomy** — live `(goodsType, sortGroupId)` classifier replaces the
  per-item `ITEM_ICONS` category column (0-drift, mod/DLC-agnostic).
- **Runtime offset resolution** — param/struct field offsets resolved live from the exe's own access
  instructions at init (AOB registry + `resolve_field_offset`) instead of hardcoded constants.
- **Loot repeatable-flag test** — live `EventFlagMan` group-allocation query replaces the numeric
  `>= 0x40000000` cut that wrongly dropped DLC one-time loot.

### Fixed
- **F1 panel mouse-wheel scrolling dead under Proton** — ImGui's only wheel source was the legacy
  `WM_MOUSEWHEEL` message, which ER's raw-input capture (`RIDEV_NOLEGACY` under Wine/Proton) never
  posts — same family as the already-polled mouse buttons/keyboard. The wheel delta is now
  harvested from the raw-input hooks (the only place it exists) and fed to ImGui each frame.
- **Grace altitude badge (▲/▼) never drew when "Gpu Sprite" was on** — `draw_marker`'s live-sprite
  grace path returned before reaching the shared `draw_altitude_badge` call; only the baked-atlas
  fallback path drew it. The altitude cue now shows on grace markers regardless of the sprite
  source setting.
- **F1 mouse position dead after Alt+Tab (round 2 — the first fix wasn't the whole story)** — a
  second, longer debugging arc found the real root cause after 4 rounds of user-tested fixes:
  `hk_get_cursor_pos` deliberately fakes screen-centre for any caller while the panel is open (to
  freeze the game's own map-panning camera), with an existing exemption flag
  (`g_imgui_reading_cursor`) for genuine reads — nothing in the cursor-tracking code was ever
  setting that flag, so every `GetCursorPos` call looked "frozen" all along. Fixed by setting the
  exemption around the real poll instead of working around a staleness that was never real. Added
  a `config::debugCursorDiagnostic` on-screen crosshair/readout (off by default) that made this
  diagnosable without another log round-trip.
- **`IniType::F32` settings silently corrupted on save+reload** — the load-time clamp was a single
  hardcoded `[0.1, 5.0]` written for the overlay scale multipliers but reused for every later F32
  field regardless of its real range: `minimap_size` (100 default) would reset to 5 on the very
  next load after any settings save, `icon_min_half_px` (8.0) similarly, and
  `grace_offset_x/y`/`minimap_offset_x/y` (0.0, needs negative values) got forced up to 0.1. Fixed
  with a real per-field min/max on each `IniEntry`.
- **Undiscovered grace icon ~2x the size of the discovered one** — both draw through the same
  destination quad size, but the discovered icon's raw screen-capture crop has more padding around
  the glyph than the hand-authored disk crop used for the undiscovered marker. Fixed with an
  automatic compensation ratio derived from each icon's own measured native pixel dimensions
  (`sqrt(w*h)` on both sides), not a hardcoded constant.
- **Overlay menu unclickable on Wine/Proton** — the F1 panel showed and hover worked, but clicks on
  buttons/sliders/dropdowns didn't register. ER reads input via Raw Input, so under newer wine/Proton no
  legacy mouse-button window messages (`WM_LBUTTONDOWN`…) are posted — ImGui's message path saw no presses
  (the cursor *position* still worked because it's polled). Mouse buttons are now polled directly
  (`GetAsyncKeyState`, like the menu toggle key) and fed to ImGui each frame, independent of message
  delivery and of fullscreen/borderless. (Confirmed: zero `WM_LBUTTONDOWN` reached the overlay while open.)
- **F1 panel cursor permanently unresponsive after Alt+Tab** — open the panel, Alt+Tab away, Alt+Tab
  back: no hover/click/move ever registered again until restart. `WM_SETFOCUS`/`WM_KILLFOCUS` were only
  forwarded to ImGui while the panel was already visible (`g_show`), which is recomputed once/frame from
  a foreground-window check — `WM_SETFOCUS` on refocus can arrive a frame before that recompute, so it
  fell through unforwarded and ImGui's internal focus-lost state never cleared. Now forwarded
  unconditionally, independent of panel visibility. (`docs/re/proton11_cursor_lock_re_prompt.md`)
- **F1 panel mouse/keyboard input lost after Alt+Tab (regression), and the item/category search
  bar could lose keyboard focus with no Alt+Tab at all.** Two distinct causes, both confirmed via
  new `[FOCUSDIAG]`/`[KBDIAG]` diagnostic logging (added this session) rather than guessed: (1)
  window-focus state (`fg`) was re-polled every present frame via `GetForegroundWindow()==g_hwnd`
  — under Wine, that call transiently returns something other than the game window for a few
  frames during the Alt+Tab compositor transition, so the poll caught those and flapped the panel
  closed/reopened several times per real Alt+Tab, resetting ImGui's focus state each time and
  leaving input dead. Fixed by tracking focus from `WM_SETFOCUS`/`WM_KILLFOCUS` messages
  (event-driven, only fire on real transitions) instead of polling. (2) The gamepad toggle-combo
  read had no debounce — a single stray frame of "combo held" (a known XInput behavior: reads
  right after an app regains focus from background can be a stale/glitchy resync burst) could
  flip the panel open/closed on its own, with no Alt+Tab involved, same focus-reset side effect on
  the search bar's `InputText`. Fixed by requiring the combo to read as held for 3 consecutive
  frames before committing the toggle. Log-confirmed fixed the flapping, but the user then found
  clicking/cursor still broken after a real Alt+Tab, root-caused via new `[KBDIAG]` logging: ImGui
  only refreshes its mouse position from `WM_MOUSEMOVE`, which this game suppresses during normal
  gameplay (raw input) — same reason the left-button click is already polled instead of read from
  `WM_LBUTTONDOWN`. `WM_KILLFOCUS` invalidates ImGui's mouse position and nothing ever refreshed
  it again, so clicks/hover never worked post-Alt+Tab even though the button poll saw them.
  Polling `GetCursorPos` alongside the button fixed that, but surfaced a second bug: the game
  keeps the real OS cursor warped to screen centre continuously during normal play (the same
  behavior already described by `hk_set_cursor_pos`'s "swallow the game's recenter-to-middle"
  comment), so the very first poll after opening genuinely read back centre, and feeding that
  stale value into ImGui showed up as a stuck/recentered cursor. **Final fix:** stopped gating F1
  on OS focus at all — `g_show` (drives drawing and every input-capture hook) now depends only on
  the F1 toggle itself, not on `GetForegroundWindow`/focus messages. Removes the focus transition
  itself instead of patching each bug it produced; user-confirmed fixed in-game. Tradeoff: F1
  stays active (including input-swallow) even if the game window loses focus — close F1 before
  Alt+Tabbing to interact with a different window.
- **Keyboard permanently dead after Alt+Tab (a separate bug from the mouse/focus fixes above)** —
  legacy keyboard window messages (`WM_CHAR`/`WM_KEYDOWN`/etc.) simply stop arriving after a real
  Alt+Tab under Wine/Proton, same `RIDEV_NOLEGACY` family as the mouse-click fix elsewhere in this
  list. Keyboard text entry now polls (`GetAsyncKeyState` + `ToUnicodeEx`) instead of relying on
  those messages while the panel is open. Known limitation: no OS auto-repeat emulation — a held
  key types once per physical press, not on a timer.
- **Minimap search-hit target vanished when outside the HUD's radius** — an item-search hit beyond
  the minimap's radius simply didn't draw; now clamped to the HUD edge along its true direction
  (like an off-screen objective indicator), and the search-hint text now says "ringed on the
  minimap" instead of "open the world map to locate them" when the minimap is already showing it.
- **Marker teleport on zoom** — overlay markers jumped for a single frame on each mouse-wheel zoom step.
  The marker motion-sync (which projects markers ~1 frame behind to ride the GFx-composited basemap) now
  delays zoom together with pan (`view_delay_zoom`, on by default); delaying pan alone left the zoom a
  frame out of step with the basemap, snapping markers radially per notch. Live-tunable via the F1
  "Marker motion delay (frames)" slider + "Delay zoom too" toggle.
- **Cross-tile false item stacks** — item stacking compared block-local positions (0–256 within a grid
  tile), so same-item markers in different grid tiles of one area merged (a Trina's Lily at Fort Haight
  stacked with one at Mistwood Ruins). Proximity now uses full area-local coords (grid·256 + pos), so
  the ~5 m radius is real distance.
- **Item-stack toggle crash** — toggling `stack_identical_items` (esp. rapidly, or with require-fragment)
  could crash (access violation in an `unordered_map` rehash). `rebuild_markers()` re-kicked a bucket
  build without waiting for the previous one, so two workers mutated `g_buckets` / a shared map
  concurrently. Builds are now serialized (single worker; a mid-build re-toggle is queued via a pending
  flag) and the worker is the only thread that clears/refills `g_buckets`.
- **Map-open freeze** — fully resolved by the ImGui/DX overlay backend: markers are no longer injected as
  native `WorldMapPointParam` rows, so the engine doesn't walk them at open. (`areaNo=99` eviction +
  clustering was the pre-overlay mitigation.)

### Performance
- **World-map marker viewport-cull** — clustered-eligible markers used to pay the per-frame visibility
  gates even when their pile cell sits off-screen (off-screen members feed the pile). Now a map-space
  viewport rect (`proj::unproject_screen` of the 4 corners, +1 tile margin) skips a clustered marker's
  gates when its 256-unit pile cell can't be on screen. `render.worldmap.markers` avg **3.58 ms → 1.28 ms
  (~64%)**, verified in-game. Provably visually invariant: a pile is drawn iff its screen-centroid is
  on-screen, and every member of an on-screen-centroid cell is within rect±256 = the margin, so no pile
  loses a member (centroid + `xN` unchanged). Added `present.frame_wall` / `present.overlay_total` bench
  timers to locate unlabelled frame cost.
- **Proton collected-refresh stutter** — dropped in-process `ReadProcessMemory`-to-self for `__try`-guarded
  noinline raw reads (read_wgm max 581ms → 4ms; killed the ~20fps stutter). Documents the clang-cl `__try`-elision trap.

### Removed
- **`quest_npc_quest_aware` toggle + the legacy quest-NPC gate** — the quest-NPC feature is now
  runtime-driven (all NPCs pinned, live state), so the broken/unfinished "Quest-aware NPCs" checkbox
  and its `QUEST_GATES`-based marker hiding were removed. `show_quest_npc` (the category toggle) stays.
- **`ITEM_ICONS` table + dead per-item icon path** — redundant with the live category classifier.
- **`_map_entries_full.cpp` intermediate** and the static map-data bake (DLL 6.19 MB → 3.76 MB).
- **5 disk loot-source toggles** — loot sources always on (breaking config change).

### Cross-platform
- **Linux/Proton build** — DLL cross-compiles on Linux via clang-cl + xwin + ninja (no MSVC/Wine).

> Known open items (tracked in `docs/memory/bugs/` and `process/`): per-tile walk-fog RE, phantom cut
> graces, the DX/bugs backlog, render-loop spatial bucketing, native-row live refresh, and the dvdbnd packed reader.
