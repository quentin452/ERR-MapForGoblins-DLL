# Changelog

All notable changes to this MapForGoblins fork are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/). Fork releases are tagged (first: **v2.1.0**,
2026-07-06); in-progress work accumulates under **[Unreleased]** until the next tag.

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

_Nothing yet — the next cycle's entries go here._

## [v2.3.0] - 2026-07-08

### Added

- Streamed far teleport: coordinate teleports beyond the streamed bubble now route through the
  game's OWN streaming instead of being refused — fast-travel to the nearest discovered grace,
  then one ground-checked hop to the exact target. New `warp_far <worldX> <worldZ>` RPC; the
  virtual-map click-to-warp falls back to it automatically when a direct hop is refused.

### Fixed

- Death marker (bloodstain, v2.1.0) was missing from the native world map — it only drew on the
  virtual map and the minimap. It now appears on the native map too, above the other icons,
  projected through the game's own converter so it lands correctly on underground and DLC pages.

- Death marker (bloodstain, v2.1.0) never appeared for a death with 0 runes, even though ER's own
  map draws its icon there — the mod treated "0 runes in the stain" as "no stain". Existence now
  follows the engine's own bloodstain flag (set on any death, restored from the save), so the
  mod's marker mirrors ER exactly, including 0-rune deaths.

- Coordinate teleport (`warp_local`/`warp_xyz`, vmap click-to-warp; v2.2.0) could drop the player
  where no ground exists — a transiently invalid player-MapId read mis-framed the delta into an
  11 km jump outside the map, and even a legitimate 40 m hop could land in a floorless deep-water
  column (mid-lake). Either way the player free-falls, and the autosave then poisons the slot so
  every subsequent load wedges on the loading screen. Three guards now apply: invalid MapId reads
  are rejected (the warp errors instead of mis-framing); the teleport LIVE-checks the target with
  a physics raycast and refuses when the column has no walkable collision (void, deep water,
  unstreamed — any distance); and when the live check can't run (loading, native map open) a
  conservative 1500 m cap applies. New `ground_at <x> <z>` RPC exposes the check for scripting.
  An already-poisoned save is repaired by restoring the newest healthy snapshot from the
  launcher's `ERR Backups/` folder next to the save file.

## [v2.2.0] - 2026-07-07

### Added

- Merchant map pins ("World - Merchants" category, on by default): every shop NPC of the ACTIVE
  install is pinned on the world map, joined at runtime from the talk ESDs (new in-DLL C++ ESD
  parser reads each `t<TalkID>.esd`'s `OpenRegularShop` range) × the MSB Enemy TalkID — fully
  mod-agnostic, no baked data (picks up ERR's own replaced/added merchants, verified offline
  38/39 + ERR extras vs the vanilla join). The F1 item search's "Sold by merchants" rows now also
  name the seller ("· sold by Twin Maiden Husks"), who is a searchable, locatable pin.

- Debug-RPC `coop` verb: co-op session diagnostics (PlayerIns count, vmap freeze-skip state, each
  partner's ChrIns + projected map position/group — the exact chain the co-op partner markers use).

- Debug-RPC `hp` verb (read / `set <v>` / `fill`) and `immortal [0|1|toggle]` dev god-mode (per-frame HP
  top-up to max). HP write reuses the `hp_probe` module chain.
- Debug-RPC `mem_write` (raw absolute in-process write) and `mem_scan_f3` (scan private rw pages for a
  live float-triplet / position copy) — runtime-RE A/B levers.

### Fixed

- Roundtable Hold markers (items, merchants, quest pins) now display on the map's Roundtable
  inset (bottom-left corner), next to the native grace icon. They used to project ~2200 map
  units off the artwork: FROM's inset icon params use hand-placed VIRTUAL coordinates, so real
  MSB positions were never meant to fold directly — markers are now remapped relative to the
  tile's nearest Site-of-Grace asset (fully live-derived, mod-agnostic). Also fixes the stacked
  interior copies drawing 100 m apart.
- Markers from a legacy map with no map conversion of its own (e.g. ERR's Roundtable copy
  m31_90) no longer pile up at an unrelated dungeon entrance (they hid the real markers there);
  such maps are treated as unmappable and their markers are skipped/hidden.

- Coordinate teleport (`warp_local`/`warp_xyz` RPC + the vmap click-to-warp) now actually moves the player.
  The old path wrote `LocalPlayer+0x6C0`, an OUTPUT MIRROR the physics thread reclaims each frame (the write
  landed but snapped back). The working teleport writes the player's HAVOK BODY Vec3 directly
  (`*(*(LocalPlayer+0x190)+0x68)+0x70/74/78`, er_console_mod's method), converted from the tile-local target
  by a frame-invariant delta. Live-verified (moved +20 m, held). Intra-region (use grace `warp` cross-map).

- `vmap graces` counted EVERY grace as discovered (it tested the discovery-flag ID instead of reading
  the event flag live) — a fresh save reported 438/438 discovered. Now uses the same live
  `read_event_flag` check as the warp gate, and the log lists all discovered graces (60-line cap dropped).

## [v2.1.0] - 2026-07-06

First tagged release of this fork. Everything below is specific to it (`master`, ~990 commits ahead of
`upstream/main`) and not present in the upstream ELDEN RING Reforged / MapForGoblins project.

### Added
- **The fullscreen Virtual World Map freezes the world while it's open (safe marker browsing).** When the vmap
  stands in for the native map (**Virtual map on map key**), opening it freezes every character — enemies, NPCs,
  the player — in pose (ER's own cutscene freeze), so you can study markers without being attacked. Closing the
  map resumes instantly, with no catch-up hitch no matter how long it stayed open. Behaviour contract:
  1. **Can't open the map once you're already in combat** — ER blocks the map in combat and the vmap inherits that
     (no vmap either); disengage first.
  2. **Open it before combat and you're protected** — nearby enemies are frozen the whole time the map is up.
  3. **Close the map at any moment** — instant, always. (This also replaced the old, unreliable "detect combat and
     force-close the map" attempt — combat detection was abandoned; freezing is simpler and correct.)
- **In-game pause is now ER's cutscene freeze (instant resume).** The "Pause game" toggle / pause-on-open / the
  `pause` RPC no longer flip the frame-step branch (whose un-pause hitch grew with how long you paused); they call
  `SetDisableAllChrUpdate`, which freezes characters with a zero-cost resume even after minutes paused.
- **The Virtual World Map fully REPLACES the native map (redirect).** With **Virtual map on map key** on, pressing
  the game's map button (keyboard OR gamepad) opens the fullscreen MapForGoblins map and the native ER world map
  **never opens at all** — the create-callback is intercepted, so there's no native map flash, no wasted render,
  and pressing the map button again closes the vmap back to gameplay. (Supersedes the earlier "draws over the
  native map" behaviour.)
- **Open the Virtual World Map on the game map key.** New setting **Virtual map on map key** (F1 ▸ Settings;
  ini `vmap_on_map_key`, off by default): when enabled, pressing the game's map button opens the fullscreen
  MapForGoblins Virtual World Map instead of the native map (it draws opaque over the native map). Off = the
  native map is untouched; the vmap still opens for custom virtual worlds and via the Dev toggle.
- **All field-boss INSTANCES now show, not just the map-marked ones.** The native ER map is selective — it
  marks only some instances of a repeated boss (e.g. 4 of the 7 Erdtree Avatars, and none underground) and
  groups some (one "Demi-Human Chiefs" icon). Boss markers are now completed from the LIVE MSB enemy scan:
  for each boss TYPE the native map marks, every other instance of it (matched by the enemy's runtime-resolved
  name, deduped per tile) gets a marker too — so item search / the map find them all. Mod-agnostic (reads the
  active install's enemy placements + NpcParam names, no bake). Only boss types already on the native map are
  completed (never a false boss). Erdtree Avatar 4 → 6 (incl. the underground one).
- **Item search distinguishes Royal vs Ashen Capital (and other pre/post story states).** A state-gated
  item now shows its game-state in the vmap item-search results — e.g. `[+] Item — Royal Capital` (reachable
  in the current playthrough state) vs `[x] Item — Ashen Capital` (needs the other state) — so you don't hunt
  an item in Leyndell Royal Capital when your save is already in the Ashen Capital (or vice-versa). Royal and
  Ashen versions of the same item split into separate rows; reachability is read live from the story flag.
- **Runtime ADD-AEG: thread wall passed + engine accepts the spawn request (dev tool).** `spawn_asset` queues
  the AEG request and drains it inside a per-frame detour on the reqMgr update `FUN_1406d31f0` (er+0x6d31f0) —
  the registrar's own main-update thread — via the native by-id helper `FUN_1406d4e80(state, aegId, worldPos)`.
  Validated live on Linux/Proton: the detour runs on the same thread as the streamer's own registrar calls
  (no deadlock/freeze), and the engine returns a nonzero handle with no fault for every asset name. The raw
  registrar `FUN_1406a5080` was proven to be an invalid cold entry (faults even with a streamer-captured name
  on the correct thread). Remaining: the accepted request isn't yet a rendered instance (`geom_stats` flat) —
  the streamer state-machine build-out. Adds diagnostic RPC verbs `spawn_capreg` / `spawn_cap4e80`.
  `docs/re/windows_geom_spawn_thread_re_findings.md`.
- **Virtual World Map auto-follows the player's dimension.** A new `Follow` toggle (on by default) switches
  the vmap page to match the dimension the player is physically in — cross from the overworld into an
  underground/DLC area and the map page changes with you. Edge-triggered on the actual crossing, so a manual
  page pick between crossings still sticks (turn `Follow` off to browse other pages freely). Reuses the live
  PlayerDim resolver; the camera is left where it is (this switches the page, not the view — the one-shot
  `Player` button still recenters).
- **Mod-agnostic catch-all category (`Loot - Other`).** A placed item MFG's live taxonomy resolves but
  can't sort into any known category now routes into a terminal `Loot - Other` category (toggle
  `show_other`, on by default) and draws with its OWN native item icon (a plain circle only when even that
  misses), instead of being dropped at marker build. Dormant on ER/ERR (every resolved item classifies);
  it's the safety net so a non-ERR mod's items never silently vanish. Award lots that resolve NO item
  (phantom/empty) stay correctly skipped — only real items reach the bucket.
- **Item search on the Virtual World Map.** The mod's own map now has its own item-search sidebar
  (`Items` toggle): type a name, get results grouped per map/page with counts, click to centre the map on
  it — no native ER map needed. The F1 "Find item / object" search also locates onto the vmap now. One
  step closer to the vmap fully replacing the native map.
- **Custom collision bodies (dev, Route D walkable greybox).** The DLL can now inject a STATIC Havok
  collision body into the live world (`add_collision` RPC, staged: resolve → cinfo dump → `go`), proven
  end-to-end with the heightfield raycast oracle (down-ray hits the new body's top; persists in the
  broadphase). First "our own asset" brick for custom 3D worlds — art-less walkable/blocking geometry.
  Shape is borrowed from a live body until the `hknpBoxShape` builder lands (half-extents recorded).
- **Death marker (bloodstain).** The map + minimap now show where you died — mirrors the game's own
  persistent bloodstain (native `MENU_MAP_DropSoul` icon), so it survives a restart and clears itself when
  you collect the runes, exactly like the base game. Overworld deaths for now (underground/legacy TBD).
- **Custom map markers (Virtual World Map).** Right-click the map to drop your own marker (a blue pin
  drawn on top of everything), and manage them from the `Custom` sidebar: each row shows which map it's on
  + its coordinates, with Go (pan the map there), TP (teleport in-game), Delete, and an editable name.
  Answers "where's my custom marker and take me there" across the overworld / underground / DLC.
- **Grace warp menu (Virtual World Map sidebar).** A `Graces` toggle opens a searchable, sorted list of
  every site of grace beside the map, each with a discovered/undiscovered state dot. Double-click a
  discovered grace to fast-travel; click any grace to pan the map to it. Browse/search/filter the whole
  grace set — impossible on the native map.
- **Virtual map terrain relief (heightfield hillshade).** The mod-owned Virtual World Map can now draw a
  mod-agnostic terrain backdrop sampled LIVE from the 3D world: a `Sample terrain` button casts a grid of
  down-rays around the player (Havok ground query on the present thread) and the map hillshades each hit
  cell (`dot(surface-normal, light)`) under the markers, toggled by `Relief`. Correct for any mod that
  reshapes the world (no baked art). Coverage is the loaded region around the player (extends via warp).
  `docs/plans/heightfield_relief_plan.md` (D2.1→D2.3).
- **Virtual map region labels (A7 parity).** The mod-owned Virtual World Map now draws the coarse
  major-region names (Limgrave, Caelid, …) on its canvas, projecting each `MAJOR_REGION_ANCHORS` anchor
  through the same live `marker_world_pos`→`w2s` transform its markers use (so a label sits over its
  region), gated to the displayed group + Base ER, with a `Labels` toggle. Closes A7 on the ImGui-only
  map parity gate (`docs/plans/imgui_only_map_plan.md`).
- **Stuck-load watchdog.** A companion to the freeze watchdog for a load that hangs instead of the
  whole game: a hung world-load keeps rendering the loading screen, so the freeze watchdog (which
  watches the render beat) never fires. The new watchdog watches the world-playable state directly and,
  if a fast-travel's load never completes within `[Debug] load_watchdog_secs` (default 30), writes a
  load-stall report + a full all-thread minidump to `logs/` so an "infinite loading" can be diagnosed.
- **F1 panel reorganized into tabs.** The one long scrolling panel is now split into five tabs by
  intent — *Markers*, *Search*, *Quests*, *Display*, *Dev* — so the everyday controls aren't buried
  under dev tooling. The settings-search box still filters across everything: while you're typing it
  hides the tabs and shows all matching blocks in one flat list, so a search never hides its own
  results behind an unselected tab.
- **Category icons in the panel.** Each row in *Markers → Sections & categories* now shows that
  category's actual map icon (the real native/disk glyph when resident, the baked atlas cell
  otherwise, a colored group dot as the universal fallback) instead of text alone, so the list reads
  like the map.
- **Auto-pause while the F1 panel is open.** New *Display* option "Pause automatically while this panel
  is open (F1 / gamepad)": opening the panel freezes the world sim (same branch as the manual *Pause
  the game* button) and closing it resumes — handy for editing/searching without the world moving.
  Only releases a pause it set itself, so a manual pause survives. Off by default (`pause_on_open`).
- **In-game World Editor (first slice).** New F1 panel section *World Editor (live)*: pick a world
  asset by id, see the loot item its map marker currently resolves to, re-skin that spot to any goods
  id, and *Refresh markers* to see it on the map — all at runtime, regulation-free, save-safe. Early
  slice (edit-by-id; a browsable picker + lot-cloning are next). Pairs with `custom_items.toml` to give
  the re-skinned item a name/stats.
- **World Editor — repoint a loot asset (slice 2).** The *World Editor (live)* panel can now point an
  asset at a **different existing loot lot** instead of only re-skinning its own lot. Non-destructive
  (leaves the shared lot untouched, so other assets on it are unaffected), with a live preview of the
  target lot's item before you commit; *Refresh markers* shows it on the map.
- **World Editor — per-slot re-skin (slice 3).** The re-skin now targets any of a lot's **8 item slots**
  (a `Slot` selector), not just slot 1, showing the selected slot's current item id live. (Slot 1 is
  what the map marker shows.)
- **World Editor — clone a loot lot (slice 5).** A `Clone this lot` button copies the current lot into a
  fresh row you can edit without touching the shared original; it pre-fills the repoint target so you
  can point an asset at the copy. *Refresh markers* now re-reads the live lot table, so a **cloned lot
  resolves on the map** (previously a newly cloned lot never showed).
- **World Editor — browsable asset/item picker (slice 6).** A `Browse (pick asset / item)` section
  scans the live params into searchable lists of pickup assets and named items — filter by name/id and
  click to fill the Asset / New-goods-id fields, instead of typing raw ids.
- **World Editor — move a placement live (slice 8).** A *Move a placement (live)* section moves a world
  object by an X/Y/Z delta using the engine's own transform setter — the object really moves and collides,
  no `regulation.bin`/MSB write. *Move this asset* targets the nearest loaded placement of the **picked
  asset** (the aegRow selected above), *Move nearest* grabs whatever's closest, *Restore* puts it back.
  Live-only (not saved). First use of the reverse-engineered geom move primitive in the UI.
- **World Editor — save edits as a world bundle (slice 7).** Every edit you make (re-skin, repoint,
  clone) is recorded; `Save bundle` writes them to `world_bundle.toml` next to the DLL, which **re-applies
  automatically on the next launch** — so an edited world persists across restarts and can be shared.
  `Apply bundle` re-runs a saved bundle live; `Clear` empties the in-memory recording. First brick of the
  runtime "World Virtualization" direction.
- **Regulation.bin-free custom items.** A new `custom_items.toml` (next to `MapForGoblins.dll`) lets
  you declare custom items as data — no `regulation.bin`, no code. Each `[[goods]]` (also `[[weapon]]`
  /`[[protector]]`/`[[accessory]]`) clones a template row from the ACTIVE install, sets fields by name,
  injects a name, and grants the item into your inventory on load — and it's **save-safe**: the item
  is stripped from the vanilla `.sl2` on every save and re-granted from the toml each launch, so the
  save stays DLL-less-loadable. Example:
  `[[goods]]\nid = 8000000\nclone = 100\nname = "Custom Item"\nfields = { sortGroupId = 101 }`.
  Mod-agnostic (clones whatever the loaded mod's template row is). See `custom_items.example.toml`.
- **Regulation.bin-free param overrides.** A new `param_overrides.ini` (next to `MapForGoblins.ini`,
  gated on `[Param Overrides] param_overrides = true`, default OFF) applies per-field param edits to
  the ACTIVE install's LIVE params at boot — so you can rebalance items/weapons/etc. **without
  shipping a modified `regulation.bin`**, touching no game files and composable with any overhaul.
  Edits are made in RAM and are **save-safe** (they reset each launch and the DLL re-applies them).
  Format `label = Param:rowId:fieldName:value`; the field offset is resolved live from the game's own
  code (version-proof, mod-agnostic — the exe has no queryable paramdef). Fields exposed so far:
  `EquipParamGoods.goodsType/.sortGroupId`, `AssetEnvironmentGeometryParam.pickUpItemLotParamId`,
  `BonfireWarpParam.textId1` (extend by adding a `FieldSpec` AOB). Dev RPC: `param_get(f)`/`param_set(f)`.
- **The ERR day/night dial exclusion is now adjustable.** The round dial region where overlay
  markers are hidden (bottom-right of the world map) was a hardcoded disc; it's now tunable via
  F1 → *UI exclusion zones* → *Edit dial* — drag the disc/time-pill handles on the open map, or set
  the `dial_disc_x/y/r` + `dial_pill_x0/y0/x1/y1` keys (1920×1080 virtual units, ERR-only). Radius 0
  hides the disc; an empty pill (bottom ≤ top) hides the pill. Save to INI to persist.
- **Markers near the dial now fade instead of popping.** Rather than hard-hiding a marker the moment
  it crosses the dial edge, its opacity ramps down across a soft band (`dial_fade_margin`, default 40
  virtual units; 0 = hard edge like before) so icons dim gradually over the dial. Works for the round
  disc too — ImGui can't clip a texture to a circle, so the fade is applied by scaling the alpha of
  each marker's vertices. Only the ERR dial fades; user-drawn exclusion rectangles stay hard.

### Fixed
- **Leyndell Ashen Capital / Elden Throne markers no longer land bottom-left off-map.** The final boss
  (Elden Beast), the Fractured Marika grace, Stakes of Marika, Summoning Pools and every other marker in a
  Leyndell sub-area (Ashen Capital / Elden Throne, map areas 19/34/35) used to collapse to the map origin
  (bottom-left corner) because those areas are dead-ends in the fold table (they have no outgoing conversion
  row). The fold now lifts such a sub-area into its parent Leyndell block and folds that to the overworld,
  so they overlay the real Leyndell location. (Verified: Elden Beast now at Leyndell instead of world (0,0).)
- **Enemy-drop loot no longer appears twice (once off-map).** An item whose lot lives in BOTH
  `ItemLotParam_map` and `ItemLotParam_enemy` (enemy/boss drops — e.g. the Banished Knight Engvall
  spirit ash) was emitted as two markers: the correct one at the enemy, plus a redundant `_map` copy
  mis-anchored to a degenerate grid in the empty map margin. The disk-loot pass now drops the `_map`
  copy when the `_enemy` award already places the lot. Gated so a lot is never lost if the enemy join
  finds no position.
- **Underground / DLC markers land in the right place on the Virtual World Map.** Base-underground
  (Ainsel River / Siofra River / Deeproot Depths, incl. the Nameless Eternal City) and DLC markers used to
  clump in the bottom-left corner of the vmap because it drew the pre-baked position, which under-/un-folds
  those layers. The vmap now re-projects them through the same live engine converter the native map uses
  (resident whenever the map is open), so they overlay their real regions — and the region-name toggles
  gate the correct area. Overworld is unchanged.
- **Overlay markers no longer punch through menus that open over the map.** When a submenu is
  stacked over the open world map (e.g. the fast-travel confirmation prompt), the overlay marker
  pass is now skipped so our post-present icons don't draw on top of it, via a live "a menu covers
  the map" game-state flag (`CSMenuMan+0x104`). Gated by the `clip_game_ui` setting.

### Removed
- **Map "UI exclusion zones" settings + the ERR day/night-dial exclusion (vmap-only collapse, phase 2).**
  Removed the F1 "UI exclusion zones (map clipping)" section — the user-drawn no-icon rectangles, the ERR
  dial placement editor, and the 10 `ui_exclusion_rects` / `dial_*` ini keys — along with the underlying
  marker soft-fade. These only hid overlay icons where they overlapped the old native map's own always-on-
  top UI; the Virtual Map owns its whole surface, so there is nothing to clip under.

### Changed
- **F1 settings declutter (vmap-only collapse, phase 1).** Retired seven native-map-only toggles now the
  Virtual Map is the map surface — `grace_overlay`, `grace_suppress_native`, `landmark_suppress_native`,
  `suppress_native_bosses`, `clip_game_ui`, and the marker motion-delay (`view_delay_frames` /
  `view_delay_zoom`). Their behavior is baked to the shipped default (overlay is the sole grace/landmark/
  boss source; game-UI clipping on; 1-frame motion sync), so nothing changes on screen — the F1 panel and
  ini just lose knobs that only made sense for the old native map.
- **Non-boss enemy names now use the game's OWN native tag, not an ImGui overlay.** The engine draws the
  red enemy-name tag from `NpcParam.nameId → NpcName` (and re-reads it live) but leaves generic mobs blank.
  MapForGoblins now resolves the mob's name from the active install and feeds it into that native path
  (inject a `NpcName` string + set the type's `nameId`), so the game renders the name itself — correct
  font/accents, frame-synced with the bar, no jitter or edge-clamp on camera swings. Replaces the old
  overlay-drawn label (removed, along with its offset/size sliders). Bosses already named are untouched;
  a mob with no name in the game data stays unnamed. Mod-agnostic (it is the engine's own data path).
  The F1 "Enemy bars" tab has per-category name filters (regular mobs / field-bosses / hostile NPCs),
  applied live — toggling a category names or un-names those enemies without a reload. (Name color is
  not offered: the native tag is always red — the engine force-recolors the field after our text, so an
  injected color can't take; coloring it would need a non-mod-agnostic HUD gfx edit.)
- **Overlay uses one font throughout (embedded DejaVu Sans), no more bitmap+TTF mix.** ASCII was ImGui's
  ProggyClean bitmap and only accents/extended glyphs were a DejaVu TTF merged on top — two rasterizers
  with different baselines, so accented chars (é in "Varré", œ, →) read raised/blurry beside the pixel
  ASCII and needed a hand-tuned offset to sit right. Now DejaVu Sans is the single font for the whole
  overlay (ASCII + extended), so every glyph shares one baseline and the raised/blurry-accent class is
  gone. Text is antialiased rather than pixel-bitmap; embedded, so identical under Wine/Proton.
- **Patch-resilience: PhysWorld + hknp body pipeline now AOB-pinned.** The last un-hardened hot RVA
  (the CS::PhysWorld singleton slot used by the terrain raycast + collision injection) and the three
  hknp body functions (cinfo init / allocateBody / addBody) resolve AOB-first with the RVA as a
  cross-checked fallback — a game patch no longer silently breaks them ([SIG] boot check: 48/48 clean).
- **Settings declutter.** Marker-rendering preferences and the minimap block moved out of the
  catch-all `[Debug]` ini section into their own `[Markers]` and `[Minimap]` sections; existing inis
  migrate their tuned values automatically (keys relocate on next launch, nothing reset). Several
  dev-era calibration sliders whose values were final (grace/native-symbol/cluster scale, altitude
  deadzone, grace draw offset) were baked to their tuned constants and removed from the F1 panel + ini
  so they can't be nudged into an ugly map; the real preferences (overall marker scale, minimap,
  legibility, altitude arrows, motion delay) stay adjustable.
- **Single DLL for every install (per-profile builds retired).** The old ERR/vanilla/erte/convergence
  DLL variants are gone: one `MapForGoblins.dll` now serves any Elden Ring install. ERR-only config
  sections/entries activate automatically when the install is ELDEN RING Reforged (runtime disk
  detection) and are force-disabled elsewhere; ERR-only data tables ship everywhere and are inert
  off-ERR. The shipped ini is the same on every package (ERR-only entries included). Also removes the
  double-DLL-load failure mode caused by shipping two profile variants side by side. One behavior
  change: the vanilla package's `live_loot_labels` (randomizer relabel) now defaults OFF like
  everywhere else — randomizer players flip that one key.

### Performance
- **Faster live map refresh (*Refresh markers*).** The marker rebuild used to re-walk every MSB on each
  refresh (~1.8s of a ~3.2s rebuild, just parsing ~480k asset placements). That geometry doesn't change
  when you edit params in the World Editor, so it's now cached and reused — a param-only refresh drops
  from ~3.2s to ~1.3s (about 60% faster) on the background worker.

### Added
- **Mob names on the enemy health bar.** The game draws a health bar for locked/aggroed enemies but
  only names bosses; regular mobs now get their name labeled over the bar too. The name is read live
  from the active install (no bundled table): the enemy's `NpcParam` name for invaders / hostile NPCs,
  the bestiary codex for generic enemies (any mod shipping one — ERR names sheep, soldiers, trolls…),
  and the boss-name band for vanilla field bosses. A mob the game genuinely has no name for stays
  unlabeled. Toggle + text-size / position sliders under F1 → "Enemy bars (mob names)"; hidden while
  the world map or a menu is open.
- **Minimap player-direction arrow.** The minimap "you are here" dot is now an arrow that points the
  direction the player is facing (read live from the character's yaw), so you can orient at a glance on
  the north-up minimap instead of guessing your heading.
- **Merchant items in the search.** The F1 "Find item / object" search now also lists items sold
  by merchants (read live from `ShopLineupParam`), including shop-only goods that have no world
  pickup — e.g. the spirit ashes and bell-bearing-unlocked stock at Twin Maiden Husks. These show
  under a "Sold by merchants" heading as info rows (no map locate yet — merchants aren't placed as
  pins), tagged "(unlock required)" when still behind an event-flag unlock. Mod-agnostic (any
  install's live shop table). Naming the seller + map pins are planned follow-ups.
- **UI exclusion zones — draw your own "no icons here" areas on the map.** New F1 section
  "UI exclusion zones (map clipping)": toggle Edit, then drag rectangles directly on the open
  world map (right-click a zone deletes it) to hide overlay icons wherever they'd cover the
  game's own UI. The ERR day/night dial (bottom-right) is excluded out of the box. Zones are
  stored in resolution-independent units (`ui_exclusion_rects`), so they hold at any display
  resolution.
- **Overlay UI localization (v1) — French included.** New ini key `overlay_language`
  (`auto`/`en`/`fr`/…): the F1 panel (settings, categories, search, clustering, quest-browser
  chrome, danger zone) and the marker-tooltip glue ("Unknown item", "3/12 left", "x4 in this
  pile", quest badges) translate via a user-editable text table `lang/<code>.txt` next to the
  DLL — a shipped `lang/fr.txt` covers ~270 strings including every category/section label.
  Missing file or string falls back to English; game CONTENT names (items, places) always come
  from the game's own files in its language. The panel's settings search matches English AND
  translated words ("echelle" finds "Marker scale"). `auto` follows the OS UI language (NB:
  under Proton that's the Wine prefix locale, often en — set `fr` explicitly). The language is
  also switchable LIVE from the F1 panel ("Language:" combo, one entry per `lang/*.txt` on
  disk) — the whole UI swaps the same frame, no restart; Save to INI persists it. Not yet
  translated: quest-browser step CONTENT and the dev-only sections.
- **`diag_boot_io` boot I/O profile (dev).** Read-only diagnostic: hooks `CreateFileW` live at the
  very top of init (before the regulation wait, not queued with the normal hook batch) and logs
  every file the process opens during boot — `[BOOTIO]` lines with time-since-arming, per-open
  latency and ok/FAIL, first ~1500 individually then counted. Correlate with the init-phase log
  timestamps to see what the startup actually waits on. Off by default.
- **Spiderfy: hover a cluster pile to fan its members out.** With clustering on, hovering a pile
  spreads its member icons around it (ring, spiral past a dozen; capped at 40 + "+N more") on a
  legibility backdrop, each with its own leg line and full hover tooltip — inspect a dense spot
  without zooming. Identical members dedupe into one icon with an xN badge, so a 50-marker pile
  fans as a readable handful. Disabled while zoomed far out (a map tile under ~64px on screen —
  the pile tooltip still works; zoom in a step to fan). The fan closes when the cursor leaves it.
  Ini `cluster_spiderfy` (Clustering, default on).
- **Pause the game from the F1 menu.** New checkbox freezes the world simulation (enemies, timers,
  physics) while the menu, map overlay and rendering stay fully usable — the pause technique from
  iArtorias' elden_pause, built in (replaces the separate PauseTheGame.dll, which can also toggle
  itself while the game is unfocused since it reads keys globally — remove it from your load order
  to avoid double-toggling). Hidden automatically if a game update breaks the signature.
- **Device-aware close-hint in the F1 panel.** The header (and the collapsed pill) now shows the
  configured gamepad combo (e.g. "Y+R3 close") while the gamepad is the active input device,
  instead of always saying "F1 close" — same detection the cursor-recenter compensation uses.
- **Dev-only debug RPC.** New ini key `[Debug] debug_rpc_port` (empty = off, the default) starts a
  TCP listener on 127.0.0.1 so an external script (`tools/mfg_rpc.py`) can drive the running game:
  `ping`/`status`, open/close the F1 panel, set any ini config key live, capture a BMP screenshot
  of the frame (overlay included), and trigger an overlay hot reload on the split build. Works from
  Linux against the game under Proton (loopback is shared). Loopback-only, no auth — dev sessions
  only.
- **Dev-only overlay hot reload (split build).** With `GOBLIN_OVERLAY_HOTRELOAD=ON` the overlay
  draw layer builds as a separate `goblin_overlay_render.dll` that the host watches and live-swaps
  on rebuild (`FreeLibrary`/`LoadLibrary` between frames, markers rebuilt automatically) — iterate
  on overlay rendering without restarting the game. Includes the /MT cross-DLL heap unification
  (render's `operator new/delete` + ImGui allocations routed to the host heap) that the split build
  needed for correctness. Shipped single-DLL builds are unaffected (option OFF = byte-identical
  path). In-game validated on ERR under Proton (3 live reloads, watcher swap ~1.3s after rebuild);
  detail in `docs/plans/overlay_hot_reload_playwright_plan.md`.
- **Per-marker native landmark glyphs** — every landmark marker now draws the game's OWN map glyph
  for its exact site (each `WorldMapPointParam` row's iconId → `MENU_MAP_<NN>`): catacomb/cave/tunnel
  icons for Dungeons, each legacy dungeon's bespoke icon, church/ruin/fort/village icons for the
  parity categories, etc. Previously only single-icon categories had a native glyph and the unions
  fell back to a circle. Mod-agnostic (resident GPU glyph → on-disk glyph → circle). Boss markers
  also gain a native glyph on installs without ERR's custom boss symbol.
- **Native landmark-pin suppression** (`landmark_suppress_native`, default on) — while a World-landmark
  category (Minor Erdtrees, Dungeons, Churches, …) is enabled, the game's own pin for those spots is
  hidden on the native world map so the overlay icon isn't a duplicate; turn the category (or the
  option) off and the native pins come back. Per-category, mod-agnostic (works on any install), takes
  effect on the next map open. Graces and boss pins are untouched (they have their own systems).
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
- **Summoning Pool / Stake of Marika / Elevator native glyphs** — Pools and Stakes both draw the
  native Marika-statue glyph (`MENU_MAP_89`, it matches both in-world objects) told apart by tint:
  pools = multiplayer blue, stakes = warm gold. Elevators draw the native lift-platform glyph
  (`MENU_MAP_21`), greyed. These POI glyphs render at a reduced per-category scale so they sit
  naturally among item icons. Resolved by iconId from the active install's map-point layout, with a
  disk (no-bake) fallback when the resident GPU symbol isn't loaded — mod-agnostic, not an ERR bake.
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
- **Markers no longer draw outside the map canvas.** When the view sits at a pan clamp (e.g. after
  a search "locate" into a fogged area) or zoomed far out, the map ART ends mid-screen — markers,
  piles, labels and fans used to keep drawing on the black void past the map edge and over the
  day/night dial. The overlay now reads the engine's own full-map rect, projects it through the
  same view as the markers, and clips every worldmap draw (and hover) to it.
- **Cluster location labels no longer flood the screen when zoomed far out.** Every pile printed
  its location name at any zoom; far out that blanketed the region in text. Labels now hide below
  the same zoom criterion the spiderfy fan uses (a 256-unit map tile under ~64 screen px); the pile
  tooltip still names the location on hover.
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
- **Overlay init no longer sleeps a flat `load_delay` after readiness is confirmed** — the init
  poll checks the real dependency (WorldMapPointParam registered with rows), so the old "honor
  load_delay as a minimum total wait" added a flat ~5s to every healthy boot for nothing. Confirmed
  ready now proceeds after a 250ms settle; `load_delay` remains as the fallback minimum only when
  the poll can't confirm. Measured in-game (ERR under Proton): `init.param_poll` **5005 ms → 250 ms**,
  DLL init complete ~10.7s after injection (was ~15s).
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
