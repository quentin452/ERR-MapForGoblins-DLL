# Changelog

All notable changes to this MapForGoblins fork are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); this fork does not yet cut
named releases, so everything fork-specific lives under **[Unreleased]** until the first tag.

## Changelog workflow

- Every completed task that **adds a feature or fixes a bug** must add a line to **[Unreleased]**.
- Group entries under the standard headings: `Added`, `Changed`, `Fixed`, `Performance`, `Removed`.
- On an official release, move the accumulated `[Unreleased]` entries under a new named version
  (e.g. `## [v1.0.0] - YYYY-MM-DD`) and leave `[Unreleased]` empty for the next cycle.
- Keep entries short and user-facing; deep technical detail belongs in `docs/memory/` and `docs/re/`.

---

## [Unreleased]

Everything below is specific to this fork (`master`, ~990 commits ahead of `upstream/main`) and
not present in the upstream ELDEN RING Reforged / MapForGoblins project.

### Added
- **Self-rendered map overlay** — all goblin markers drawn by an in-process ImGui/DX12 overlay
  projected onto ER's world map, replacing native `WorldMapPointParam` injection (the sole shipped map path).
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
- **Map-open freeze** — fully resolved by the ImGui/DX overlay backend: markers are no longer injected as
  native `WorldMapPointParam` rows, so the engine doesn't walk them at open. (`areaNo=99` eviction +
  clustering was the pre-overlay mitigation.)
- **`require_map_fragments` leak** — interior overworld tiles inherit the majority fragment of their 8 neighbours.
- **Page-transition flicker** — re-seed the view-delay ring buffer on page-group change.
- **Gamepad / mouse-still map drift** — projection re-centred on the cursor-independent pan/zoom midpoint.
- **DummyAsset over-emission** — disk loot walk drops MSBE part-type 9 (kept only when entity-bound).
- **Disk-parser coverage gaps** — shared `emit_lot_siblings()` across treasure / enemy / EMEVD passes.
- **Clustering live-test bugs** — location-anchored clusters with live re-plan on toggle.

### Performance
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
