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
- **Item / object search bar** — F1 search that locates a marker by name and pans+zooms the live map onto it.
- **Quest Browser** — in-overlay ordered per-NPC step list with persisted checkmarks, missable warnings,
  and grey-out of unfinishable questlines from EMEVD-derived death flags.
- **Quest-aware NPC marker layer** — 344-marker WorldQuestNPC layer with optional quest-active gating.
- **Coverage-gap detector** — opt-in SetEventFlag + AddItemFunc hooks that toast "unmapped item collected".
- **Live world→map projection** — engine-native projection reverse-engineered and wired (`liveProjection`),
  fixing hundreds of misplaced markers; live "you are here" player position on every page.
- **Native map-point icons** — Oodle-IAT-hooked DDS harvest draws the game's real grace/boss pins on markers.
- **Unicode overlay font** — embedded DejaVu Sans TTF (Latin-Ext / Greek / Cyrillic) over ImGui's Latin-1 default.

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
- **`ITEM_ICONS` table + dead per-item icon path** — redundant with the live category classifier.
- **`_map_entries_full.cpp` intermediate** and the static map-data bake (DLL 6.19 MB → 3.76 MB).
- **5 disk loot-source toggles** — loot sources always on (breaking config change).

### Cross-platform
- **Linux/Proton build** — DLL cross-compiles on Linux via clang-cl + xwin + ninja (no MSVC/Wine).

> Known open items (tracked in `docs/memory/bugs/` and `process/`): per-tile walk-fog RE, phantom cut
> graces, the DX/bugs backlog, render-loop spatial bucketing, native-row live refresh, and the dvdbnd packed reader.
