# Settings sweep — classify every F1/ini knob (keep / hardcode / dev-quarantine / delete)

Status: **Phase 0+1+2 LANDED + IN-GAME CONFIRMED; Phase 3 LANDED (build-verified, in-game verify
pending).** Author 2026-07-02.

### Phase 3 (commits 369b619 grace, 360e3ab projection)
- **FLAG-2 grace baked-atlas — DELETED.** graceGpuSprite hardcoded true; the CPU baked-atlas grace
  draw branch removed; sprite-not-harvested falls to the universal circle (native → circle, no baked
  middle tier). Config var dropped everywhere; the graceOverlay=OFF path keeps its "show_graces"
  atlas key. **In-game verify pending:** discovered/undiscovered graces still correct, no vanish.
- **FLAG-4 live projection — knob REMOVED, fallback KEPT (correction to the original plan).** The
  baked projection is NOT fully deletable: the engine ships no converter for m19 Chapel + DLC
  unplaced areas, which rely on the baked affine, plus the pre-map-open window. So liveProjection is
  hardcoded true (always try live) and the baked `world_to_mapspace`/LEGACY_CONV fallback stays as
  the fallback within project_marker/project_raw. Only the config knob + branch gate + F1 checkbox
  removed. **In-game verify pending:** dungeon/UG/DLC/Chapel placement unchanged, no pop-in.

### What landed (commits 3368a20 + this one)
- **Phase 0** — ini migration handles cross-section key MOVES (global-unique-key scan): a relocated
  key is adopted from wherever it sits in an existing ini instead of orphaned + reset. `goblin_config.cpp`.
- **Phase 2** — hardcoded the final-calibration sub-knobs to `constexpr` in `map_renderer.cpp`
  (kGraceIconScale kept SEPARATE = vanilla parity, kMapSymbolScale, kClusterScale, kAltitudeDeadzone);
  deleted grace_offset_x/y (no-op). Dropped from config struct + schema + render-api X-macro + panel + fr.txt.
- **Phase 1** — new `[Markers]` (master/icon scale, legibility, icon_min_half_px, altitude_cue,
  view_delay_*) + `[Minimap]` ini sections; both moved OUT of the `[Debug]` dumping ground. `[Debug]`
  now holds only genuine dev/diag/RE knobs.

### Remaining (deferred, lower value / higher risk)
- **In-game migration confirm — DONE (user, 2026-07-02): confirmed working in game.** Existing ini
  migrated its tuned values into the new `[Markers]`/`[Minimap]` sections, no reset, no dup/orphan keys.
- **[Goblin] diag-key relocation** — move the 5 `diag_*` + `debug_logging` (+ the `baked_only` DIAG)
  out of `[Goblin]` into `[Debug]`. Deferred: non-contiguous in the schema, self-labeled already,
  brace-risk > value. The Phase-0 migration will move their values for free when done.
- **Panel dev-widget moves** — the "Baked-only (diag)" checkbox in General settings + the 4 `DEBUG:`
  checkboxes inline in Clustering should move to a dev-gated panel area (cosmetic, panel-only).
- **Phase 3 structural deletes — DONE 2026-07-02** (commits 369b619 + 360e3ab; in-game verify
  pending). Grace atlas fully removed; live-projection knob removed but the baked projection FALLBACK
  is kept (load-bearing for unplaced areas — see Phase 3 note at top).

Owner branch when work starts: fork `chore/settings-sweep` from master.

## Why

The F1 panel + `MapForGoblins.ini` carry a mix of (a) real user preferences, (b) sliders whose
values are now **FINAL CALIBRATION** (exposing them invites breaking the look — e.g. icon scaling can
make the map ugly), (c) dev/diag/RE knobs, and (d) dead leftovers. Goal: classify every widget + ini
key into those 4 buckets, then keep / hardcode / quarantine / delete accordingly.

### Hard constraints (user, do not violate)

- **Graces have a SEPARATE scale ON PURPOSE.** `grace_icon_scale` (1.2) is calibrated for VANILLA
  PARITY when the cursor locks a grace. **Do NOT fold it into the generic icon scale.** If it's
  hardcoded, keep it a distinct constant, not `overlayIconScale`.
- Any calib knob tied to a still-OPEN polish bug must NOT be hardcoded before that bug lands (its
  value may still need tuning). See "Interlock with open backlog" below.

## The core finding

Two ini sections are dumping grounds:

- **`[Debug]`** holds real dev/diag keys AND *all* the final-calibration values — every marker scale,
  the whole minimap block, altitude, legibility, view-delay. A user opening the ini sees calibration
  constants filed under "Debug".
- **`[Goblin]`** mixes real preferences (require_map_fragments, collected_graying…) with 5 RE-diag
  keys (diag_loot_flags/pos, diag_map_opens, diag_fieldins_join, diag_lot_memscan) + debug_logging.

Panel side is cleaner — dev is mostly already quarantined into `panel_dev_icons.cpp` (behind
`dump_icon_textures`) and `panel_dev_tools.cpp` ("Dev Tools" section, save+restart). **Leaks into
user-facing panels:** the "Baked-only (diag…)" checkbox in *General settings* (un-`tr()`'d), 4 `DEBUG:`
checkboxes inline in *Clustering*, and the calib sliders (grace offset X/Y, motion-delay) in *Marker
scale*.

## Edit surface (per knob dropped/moved)

Dropping or renaming one config var is NOT a one-liner. Touch points:

1. `src/goblin_config.hpp` — the `extern` decl in `namespace config`.
2. `src/goblin_config_schema.cpp` — the struct default, the `emit_ini` line, the parse line.
3. `src/goblin_overlay_render_api.hpp` — the `X(name)` entry in the render→host X-macro list
   (lines 41-47) IF the var is `GOBLIN_RENDER_API` (all the scale/minimap/calib ones are). The
   hotreload split breaks if a `cfg_*_ptr()` accessor is generated for a var that no longer exists.
4. call sites — mostly `src/worldmap/map_renderer.cpp` + `category_meta.cpp` (replace
   `*cfg_X_ptr()` with the hardcoded constant).
5. `src/overlay_panel/*.cpp` — the widget.
6. `assets/lang/fr.txt` — the label translation (drop the `en=/tr=` pair).
7. `src/goblin_config.cpp` `config_set_by_key` — the RPC `set` branch (if present).
8. docs: `docs/changelog.md` if user-facing, the relevant `docs/memory/` note.

A pure **move** (calib out of `[Debug]` into a real section) is lighter: only #2 (emit section) +
`config.cpp` migration rename map, so old inis get the key relocated on load, not orphaned.

---

## Classification

Legend: **(a)** keep = real preference · **(b)** hardcode = drop knob, bake current value ·
**(c)** dev = collapse into one dev-gated section · **(d)** delete = dead · **MOVE** = keep the knob
but relocate to a correct ini section · **⚠FLAG** = needs user decision (below).

### `[Goblin]` section

| key | bucket | action |
|-----|--------|--------|
| load_delay | a | keep (fallback boot wait) |
| clip_game_ui | a | keep |
| ui_exclusion_rects | a | keep (data, not a widget) |
| require_map_fragments | a | keep |
| baked_only | **c** | move panel checkbox out of General → dev section; keep ini key dev-only |
| collected_graying / hide_collected | a | keep |
| stack_identical_items | a | keep |
| show_region_labels | a | keep |
| native_item_icons | a | keep (no widget currently — consider exposing or drop; **⚠FLAG-1**) |
| diag_loot_flags / diag_loot_pos / diag_map_opens / diag_fieldins_join / diag_lot_memscan | **c** | move to a `[Dev]` section (or gate emit on debug_logging) |
| debug_logging | **c** | keep but move to `[Dev]` (it's the dev master gate) |
| loot_msb_dir | a | keep (mod-agnostic override, real use) |
| drop_merchant_phantoms | a | keep |
| show_all / show_all_except | a | keep |
| icons_hidden | a | keep (persisted master) |
| overlay_toggle_key / overlay_toggle_gamepad | a | keep |
| overlay_language | a | keep |
| virtual_keyboard_layout | a | keep |
| quest_progress / region_toggles | a | keep (persisted state, not widgets) |

### `[Display Sections]` — all (a) keep (the 7 group toggles).

### `[Quest Browser]`
| grey_unfinishable_on_death | a | keep (EXPERIMENTAL but user-facing, has a toggle) |
| quest_allow_flag_write | a | keep (cheat gate, intentionally opt-in, in Dev Tools) |

### `[Clustering]`
| enable_clustering / cluster_spiderfy / cluster_hard / cluster_threshold | a | keep |
| cluster_distance_adaptive / cluster_near_threshold / cluster_near_radius / cluster_far_radius | a | keep |
| cluster_exclude / cluster_threshold_overrides | a | keep |
| cluster_debug_radius / cluster_debug_markers | **c** | the 2 inline `DEBUG:` checkboxes → dev section |
| (debug_cluster_anchors / debug_region_volumes live in `[Debug]` but their widgets are in Clustering panel) | **c** | same — dev section |

### `[Equipment]/[Key Items]/[Loot]/[Magic]/[Quest]/[Reforged]/[World]` category toggles
All **(a) keep** — these are the whole point of the mod. ~90 `show_*` keys. No change except:
- `landmark_suppress_native`, `hide_killed_bosses` (in `[World]`) — (a) keep.

### `[ERR Markers]` — all (a) keep (ERR-only, force-disabled off-ERR already).
- `grace_gpu_sprite` — (a) keep BUT **⚠FLAG-2**: baked-atlas vs live-sprite is a dev A/B; the live
  sprite is "validated working" per config comment. Candidate to hardcode true + drop the CPU path.

### `[Compatibility]` — live_loot_labels, anonymous_loot → (a) keep.

### `[Debug]` — THE dumping ground. Split three ways:

**→ stays dev (c), collapse into one `[Dev]` section, emit gated on debug_logging:**

| enable_marker_dump, marker_dump_key | c | dev |
| debug_rpc_port | c | dev (keep — the RPC loop needs it) |
| freeze_watchdog_secs | c | dev (but keep default 20 — it's a safety net; **⚠FLAG-3** maybe (a)) |
| diag_boot_io | c | dev |
| debug_event_flags, debug_item_grants, debug_flag_capture | c | dev (Dev Tools) |
| debug_worldmap_probe, debug_page_switch, dump_icon_textures, dump_converters, dump_native_pins, overlay_markers_proto, debug_render_dims, debug_cursor_diagnostic | c | dev RE probes |
| probe_field_access, probe_field_spec | c | dev RE tool |
| bench_log_individual, bench_log_session | c | dev (default true — keep behavior; move to [Dev]) |
| debug_cluster_anchors, debug_region_volumes | c | dev viz |
| live_projection | **⚠FLAG-4** | default **true**, NOT really debug — it's the real projection path. Likely (a) keep-but-rename, or hardcode true + drop the baked-projection fallback. |
| fix_midsession_resolution | c or d | EXPERIMENTAL, default off — **⚠FLAG-5** keep-as-dev vs delete |

**→ final calibration (b), hardcode + drop knob (values in parens are current tuned):**

| grace_icon_scale (1.2) | **b** | hardcode as its OWN constant (vanilla-parity — the hard-constraint one) |
| map_symbol_scale (2.2) | b | hardcode |
| overlay_cluster_scale (1.0) | b | hardcode |
| grace_offset_x (0.0) / grace_offset_y (0.0) | **b/d** | values are 0 = no-op calibration probe → delete outright |
| altitude_deadzone (5.0) | b | hardcode |
| icon_min_half_px (8.0) | **⚠FLAG-6** | tied to OPEN polish item 5 (golden runes too small) — do NOT hardcode yet |
| view_delay_frames (1.0) / view_delay_zoom (true) | **⚠FLAG-7** | tied to OPEN polish item 3 (zoom+pan dash) — keep as dev knob until item 3 lands |
| overlay_master_scale (1.0) | **⚠FLAG-8** | accessibility "make all icons bigger" = arguable real preference vs calib |
| overlay_icon_scale (1.2) | **⚠FLAG-8** | same axis as master; HANDOFF says touching icon scale makes map ugly → lean (b), but overlaps master |
| icon_legibility (true) / altitude_cue (true) | a | keep — these are real on/off preferences (already have panel checkboxes) |

**→ real preference (a) MISFILED, relocate out of `[Debug]` into a proper section:**

| whole minimap block: show_minimap, minimap_zoom, minimap_size, minimap_opacity, minimap_anchor_right, minimap_anchor_bottom, minimap_offset_x, minimap_offset_y | a | **MOVE** to a new `[Minimap]` section |

---

## Flag resolutions (user, 2026-07-02) — RESOLVED

- **FLAG-8 → RESOLVED: keep BOTH.** `overlay_master_scale` + `overlay_icon_scale` stay user
  sliders (accessibility — make all icons bigger/smaller). Marker-scale REMAINS a user panel. Only
  the sub-knobs get hardcoded: `grace_icon_scale`, `map_symbol_scale`, `overlay_cluster_scale`,
  `altitude_deadzone`, `grace_offset_x/y`.
- **FLAG-2 → RESOLVED: hardcode `grace_gpu_sprite = true` + DELETE the baked-atlas CPU grace path.**
  Now in-scope: remove the CPU baked-atlas grace draw code, not just the knob. Aligns with the
  kill-baked prime directive. Bigger than a knob drop — find + delete the `!graceGpuSprite` branch.
- **FLAG-4 → RESOLVED: hardcode `live_projection = true` + DROP the baked projection fallback.**
  Now in-scope: delete the baked LEGACY_CONV+affine+DLC-eyeball fallback path used when
  live projection is off / map closed. Load-bearing code — do this carefully, its own commit,
  in-game verify dungeon/UG placement unchanged. Biggest single cleanup in the sweep.
- **FLAG-1 → default: keep `native_item_icons` as silent ini-only** (true, works, no widget needed).
- **FLAG-3 → default: keep `freeze_watchdog_secs`** as a safety net; relocate to `[Dev]` but keep it
  settable (not hardcoded).
- **FLAG-5 → default: keep `fix_midsession_resolution` as dev** (experimental, off; cheap to leave).
- **FLAG-6/7 → confirmed: `icon_min_half_px` + `view_delay_*` stay knobs** until polish items 5 & 3
  land (see interlock below).

## Interlock with open backlog (docs/HANDOFF.md "Overlay polish batch")

Do NOT hardcode these while their bug is open — the knob is the fix's tuning surface:
- item 3 (zoom+pan 1-frame dash) ↔ `view_delay_frames` / `view_delay_zoom`.
- item 4 (legibility disc on dark minimap) ↔ `icon_legibility`.
- item 5 (golden runes too small) ↔ `icon_min_half_px`.
Sweep the unambiguous knobs first; revisit these after their polish item lands.

## Execution order (buckets + flags all resolved — ready to implement)

1. **Relocate-only pass** (cheap, no behavior change): new `[Dev]` + `[Minimap]` ini sections; move
   the misfiled keys via the `config.cpp` migration rename map so old inis relocate on load. Move the
   4 stray dev widgets (baked-only, 2 cluster DEBUG, cluster-anchor/region-volume) into a dev panel.
2. **Hardcode pass (b):** grace_icon_scale (own const, vanilla-parity), map_symbol_scale,
   overlay_cluster_scale, altitude_deadzone. Per the edit-surface checklist. KEEP overlay_master_scale
   + overlay_icon_scale as user sliders (FLAG-8).
3. **Delete pass (d):** grace_offset_x/y (no-op). Then the two structural removals (own commits each,
   in-game verified):
   - **grace baked-atlas CPU path** (FLAG-2): hardcode graceGpuSprite=true, delete the `!gpuSprite`
     grace draw branch + baked grace atlas plumbing.
   - **baked projection fallback** (FLAG-4): hardcode liveProjection=true, delete the baked
     LEGACY_CONV+affine+DLC-eyeball fallback. Load-bearing — verify dungeon/UG placement unchanged.
4. Build clean (both configs), in-game spot check (ini migrates, map look unchanged, graces + dungeon
   markers still correct), `docs/changelog.md` + `docs/memory/` update, commit.

Do steps 1–2 first (low-risk, reversible); the FLAG-2/FLAG-4 structural deletes in step 3 are the
risky ones — each its own commit so a regression bisects cleanly.

## Numbers

~150 ini keys total (~90 are category `show_*` = all keep). Non-category knobs: ~60.
Rough bucket split of the ~60: **a** ~28 · **b** ~7 · **c** ~22 · **d** ~2 · **⚠FLAG** ~8.
