---
name: overlay-item-search-bar
description: The F1 "Find item / object" search bar — results list + map ring-highlight + cursor-locate (cursor = the 2D map camera). API + the perf rule + the cursor calibration caveat.
metadata:
  node_type: memory
  type: project
---

**Shipped 2026-06-26 (branch feat/phase3-taxonomy-classifier, commit dd566c7, built clang-cl +
deployed; awaiting <user> runtime-test).** A second search box in the F1 panel, DISTINCT from the
existing category-search (which filters the toggle list): this one finds MARKERS by item name.

**Behaviour:** type a name fragment → a results list (name + marker count); matching markers are
ringed (yellow circle) on the map AND pulled out of cluster piles so each shows individually; click
a result → the OS cursor is pointed at that marker. In ER's 2D world map the **cursor = the camera**
(<user>: "le cursor = camera, il suffit de pointer le bon curseur"), so this scrolls the map onto
the item — no native-map-camera RE needed.

**Where:** UI block in `goblin_overlay.cpp` (the F1 panel, just before "Sections & categories").
Renderer support in `map_renderer.{cpp,hpp}`:
- `set_item_search(const std::unordered_set<int32_t>* matchNameIds, int32_t locateNameId)` — hand it
  the set of matching marker name_ids each frame (null/empty = inactive) + a latched locate request.
- `take_locate_pos(float* x, float* y)` — after render_markers, returns the located marker's
  backbuffer/client-px pos once; the overlay maps it via `ClientToScreen(g_hwnd)` + the real
  `o_set_cursor_pos` (SetCursorPos) so the game's per-frame cursor recenter doesn't swallow it.

**THE PERF RULE:** the match set is rebuilt ONLY when the query string changes (scan `overlay_layers()`,
resolve each distinct name_id once via `lookup_text_utf8`, substring-match). The hot render loop
(~8477 markers/frame, see [[overlay-render-perf-followups]]) then does just an O(1) `set::count` per
marker. NEVER resolve names per-frame in the loop.

**LOCATE = PAN THE MAP (revised, commit 8ab5c56).** The first approach (SetCursorPos to nudge the OS
cursor) did NOT move the view → replaced. Click-a-result now PANS the live map: the renderer captures
the located marker's marker-space coord (gU,gV); `worldmap_probe::set_view_center(mU,mV)` writes
WorldMapArea pan (+0x378/+0x37C) via the engine's own inverse `pan = viewCentre·zoom − snapMid`
(same as goblin_projection ViewDelay lines 110-111 — NOT a guess). Only valid for the OPEN page.

**REVEAL HIDDEN HITS (commit 8ab5c56):** a search hit now draws even with its category toggled off OR
the icon master off — `render_markers` folds the master into per-layer visibility (`master_on &&
L->visible()`) but keeps iterating hidden layers while `item_search_active()`, drawing only the hits.
`draw_worldmap_markers` no longer early-returns on master-off when a search is active.

**PAGE-AWARE (commit 8ab5c56):** results list labels each hit's page (Overworld/Underground/DLC from
`Marker.group` bits: bit0=UG, bit1=DLC) + tooltips off-page hits "switch to that page". The render
loop gates `m.group != open_grp` so off-page hits can't be ringed/located until the user switches.

**LOCATE PICKS THE BEST INSTANCE (commit 814cb98):** a searched name can have many markers; the
locate scores every on-page match → pans to UNCOLLECTED-first, then nearest to the view centre
(`marker_done` + `(pan+snapMid)/zoom`). All-collected → nearest (greyed). De-clustering already
handled (`clustered_eligible = clustering && !is_hit`).

**CROSS-PAGE = PERSISTENT LOCATE (commit 6cef7ae, bug 1):** clicking a result whose marker is on
another page (Underground/DLC) KEEPS the locate pending (`s_locate_nameid` not cleared when no on-page
candidate) and pans the instant the user switches to that page. `locate_pending()` drives a yellow
"switch to the <page> map" banner. We do NOT auto-drive the native page swap.
**AUTO-PAGE-SWITCH SHIPPED (experimental, commit 77f0651, awaiting runtime test).** Clicking a
cross-page result now switches to its page automatically, then the persistent locate pans. Pinned by
the **[PAGESW] instrumentation** (config `debug_page_switch`, goblin_worldmap_probe.cpp `ps_detour<N>`;
findings in docs/re/windows_worldmap_page_switch_re_findings.md): of the 8 hooked candidates only
**`c1fc0` @ 0x9c1fc0 = base↔DLC** (arg2 = page 0/10) and **`c7900` @ 0x9c7900 = surface↔UG** (arg2 =
layer 0/1) fire; `c40f0` (the page-transition doc's claimed surface↔UG) NEVER fired — doc was wrong.
**Thread-safe by construction:** the handlers run on the game UI thread, so we HOOK the per-frame map
step `FUN_1409c32f0` @ 0x9c32f0 (game thread, receives dialog) and drain a pending `request_switch_to_
page(group)` (atomic) INSIDE it — one axis/step (page then layer). The c32f0 detour types dt as
**float** (xmm1) to preserve it across the forward call. dialog = active_cursor − 0x2DB0.
**page_selectable = GRACE DISCOVERY (commit 50b530f, <user>'s idea — robust, save-backed).** The
dialog-byte approach was ABANDONED: DLC=`dialog+0x27c8` (RPM-diffed across 3 saves: OW=0/UG-only=0/
all=1, with `0x3e69` an identical parallel pair) read UNRELIABLY at runtime (greyed even on a
discovered save), and the UNDERGROUND flag was never found (contextual). Instead: **no grace rested in
a region (DLC bit2 / underground bit1) = never been there = its map is undiscovered.** The overlay
scans grace markers (`m.discover_flag` + `m.group`, `read_event_flag`, cached/throttled, recomputed
only while false → `s_dlc_seen`/`s_ug_seen`); greys + disables "[undiscovered]" results + won't request
the switch. Covers BOTH DLC and underground. TODO(page_og_underground_available) in the probe header if
a clean dialog flag is ever wanted. Tooling for that: <ghidra_scripts>\page_avail_dump.py /
page_full_dump.py (cursor from wmprobe log, dialog=cursor−0x2DB0, diff full 0x4000 dumps; §6 findings).
**Switch handlers pinned:** c1fc0@0x9c1fc0 (page, arg2=0/10) + c7900@0x9c7900 (layer, arg2=0/1); the
c32f0 marshal drives BOTH axes (requesting an off-page group also toggles the layer back to surface →
the "Godrick searched from the underground" fix). The page-transition doc's c40f0 guess was WRONG.

**F1 FOCUS-GUARD (commit 6cef7ae, bug 2):** `GetAsyncKeyState(toggleKey)` is GLOBAL → pressing F1
while alt-tabbed/off-screen armed the cursor hooks off-screen. Fixed: `g_show = g_user_show && fg`
(fg = game window foreground); toggle gated on fg; close-time re-clip only on a real close (fg), not
focus loss. This was PRE-EXISTING (g_show-gated SetCursorPos/ClipCursor hooks), surfaced via the
search work.

**F1-OPEN NAVIGATION SOLVED (commit 3007da9, <user>'s diagnosis).** The map only switched/centred
after CLOSING F1 because the ImGui panel's input hooks BLANK ER's input — and ER drives the 2D map
camera off `GetCursorPos` (frozen to screen-centre to stop drift) + raw input. With no perceived input,
ER never STEPS its map, so our layer-byte write + pan don't apply (the DLC page worked because `c1fc0`
is a self-applying function CALL; a data write needs ER to process it). FIX: a nav window
`g_nav_frames` (set ~90 on a result click, counted down each Present) during which BOTH `hk_get_cursor_pos`
AND `hk_get_raw_input_data` inject a **±1px net-zero jitter** instead of freezing/zeroing → ER sees the
cursor move → steps the map → applies the switch+pan, F1 still open, no visible drift. Paired with the
pan-HOLD (re-apply set_view_center ~45 frames, since the freeze re-asserts a static view). Marshal also
got a ~2s give-up so a switch that never lands can't pin page_switch_busy() and block all pans.
**Layer switch = direct byte write** `[dialog+0x2B68]+0xB8 = wantLayer` (seh_write_u8): the [PAGESW]
log proved NO hooked handler does UG→surface (c7900 only surface→UG; the reverse byte 1→0 is unhooked),
so we write the flag and the woken c32f0 step cross-fades.

**ALL 3 TODOs DONE + RUNTIME-VALIDATED + MERGED TO MASTER (merge eabe097, pushed 2026-06-26;
feat/phase3-taxonomy-classifier: c865911 zoom + 98e766d grace-gate + 3eeabdf edge-centring + 3a4afff
doc). <user>: "Item browser functionally works" / "working perfectly". The whole F1 item/object
browser + Phase-3 taxonomy classifier shipped to master.**
1. **OOB / edge centring — FIXED STRUCTURALLY (commit 3eeabdf).** Symptom: locating a marker near a
   world edge (Godrick @ Stormveil) didn't move the map AT ALL. The OOB-pan-clamp theory was WRONG
   (proven by a live "Locate debug" overlay: the clamp never triggered — Godrick is interior). REAL
   cause: the engine DERIVES the pan from the cursor reticle every game frame — the per-frame step
   FUN_1409c32f0 eases pan toward dialog+0x2EAC (= cursor+0xFC), see windows_worldmap_viewcenter_re_
   findings.md §5b. So a Present-thread pan write AND a reticle (+0xFC) write are both reverted next
   frame (the debug showed LIVE pan == reticle·zoom − snapMid, never our value). FIX: we already hook
   c32f0 (the page-switch marshal, game thread); the locate now calls set_locate_target(marker) and
   hk_c32f0 writes the cursor reticle (+0xFC/+0x104/+0x10C pairs) to the target BEFORE the original step
   → the engine's own easer pans onto it, and being the last write before the easer (game thread) it
   sticks. Held ~90f with the nav jitter kept alive (so c32f0 keeps stepping with F1 open); clear on end.
   set_view_center no longer writes pan/reticle from the render thread (futile) — just the zoom-in + a
   (now vestigial) world-clamp + the debug snapshot. KEY LESSON: don't fight the engine's per-frame
   cursor→pan derivation with memory writes — hook the per-frame step (c32f0) and write the reticle
   target there. The "Locate debug (dev)" panel (gated behind Verbose logging) is the tool for any
   future locate issue: shows LIVE view vs target + "live-vs-target dPan -> CENTERED OK".
2. **Zoom at centring — DONE.** `set_view_center(mU,mV,minZoom=0)` now ZOOMS IN if the live view is
   more zoomed-OUT than minZoom (writes VIEW_ZOOM +0x380 directly = snap; never zooms out; pan computed
   with the post-zoom value since snapMid is zoom-independent). The locate hold passes `kLocateZoom=0.5f`
   (calibration const in goblin_overlay.cpp; map zoom ~0.05..1.0, kGraceZoomRef=0.25 mid — bump if still
   too wide). Per-frame hold re-asserts it so it self-corrects any engine ease lag.
3. **Grace-gate — "never blocks" BUG FIXED + RUNTIME-VALIDATED (<user>: "fully fixed").** <user>: "I rested at NO grace yet a
   non-discovered page never blocks" + "the per-grace green check works fine." Investigated: the green
   check (map_renderer draw_marker ~L295/329) and the gate scan BOTH read `read_event_flag(m.discover_flag)`
   where `discover_flag = BonfireWarpParam.eventflagId` (grace_layer L52; reader orp_flag_set is fail-CLOSED)
   — there is NO flag discrepancy in the source (<user> suspected one; there isn't). ROOT CAUSE: `s_dlc_seen`/
   `s_ug_seen` were function-local `static` that LATCHED true and never reset → they outlive the DLL across
   a save/character switch in one game session, so a fresh save kept the prior save's "page unlocked".
   FIX (built+deployed, awaiting runtime): don't latch — recompute seen LIVE every throttled tick from the
   precomputed per-group grace-flag lists (s_dlc_grace_flags/s_ug_grace_flags; flag IDs are save-independent
   so built ONCE, discovery state re-read each tick — tiny list, free). NOTE the gate is still PAGE-level
   (one discovered UG grace unlocks the WHOLE underground page incl. Deeproot etc.) — if <user>'s "non-
   discovered page" is really a sub-REGION he's visited the page of, this won't block it; that needs the
   region-level upgrade (nearest_region + per-region grace discovery, ERR-only). Couldn't RPM-confirm which
   case (eldenring.exe not running this session).
Other minor: off-screen matches aren't ringed (culled pre-draw). Content-agnostic → randomizer foundation.
See [[phase3-taxonomy-map-validated]], [[ghidra-worldmap-re]].

**MULTILINGUAL SEARCH + ORDER-INDEPENDENT MATCHING (2026-06-27, branch feat/i18n-item-search,
MERGED to master — merge 6f7b5d5, NOT pushed; <user> runtime-confirmed "work").** The search matched
only `lookup_text_utf8(name_id)` = the live PlaceName label, which is in the GAME's language → a
French (or any non-EN) player couldn't find a marker by its English/wiki name. Two parts:
- **English alias index** = NEW generated table `src/generated/goblin_name_aliases_en.{hpp,cpp}`
  (`NAME_ALIASES_EN`, sorted-by-id, binary-searched via `goblin::lookup_name_alias_en_utf8(id)` in
  goblin_messages.cpp). Generated by `tools/generate_data.py::generate_name_aliases_en_cpp` (err=3276
  entries), keyed by the SAME encoded `Marker::name_id` and aggregating every committed English source:
  items `items_database.json` (encode_live_item offsets 500M/100M/200M/300M/400M), NPCs
  `npc_name_text_map.json` (+700M), enemies/Codex `enemy_names_i18n.json` engus (+900M), bosses
  `boss_list.json` `wmpTextId1→vanillaPlaceName` (raw). Guard: drop encoded ids outside int32 (the
  INT32_MAX "ERR dummy" NpcName sentinel overflowed). Names are English because the whole pipeline reads
  the engus FMGs. Search loop matches FR OR EN; label shows "<game-lang> (English)" when they differ.
- **Token (AND) matching** (`matches_all_tokens` in goblin_overlay.cpp, replaced the whole-query
  `contains_ci`): split the query on whitespace, EVERY token must appear (fold_ci = case+accent-
  insensitive, already existed) in the marker's combined `loc + " " + en` text → word ORDER no longer
  matters ("Claw Talisman" == "Talisman Claw") and FR/EN words can be mixed. Fixed <user>'s report that
  one word order found nothing.
- Scope = PLACED/baked markers; a name_id with no alias falls back to game-language-only (no regression).
  Build gotcha hit: see [[build-toolchain-clang-xwin]] (cmake `-D` args MUST go via a PS array).