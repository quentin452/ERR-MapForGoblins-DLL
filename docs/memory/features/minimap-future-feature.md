---
name: minimap-future-feature
description: Future feature idea — in-game minimap overlay (corner HUD) reusing the worldmap overlay infra + the now-live player world position
metadata: 
  node_type: memory
  type: project
---

## Idea (user 2026-06-21): in-game MINIMAP overlay

**✅ GPU/NATIVE ICONS WORK IN THE MINIMAP — VERIFIED IN-GAME (2026-06-23, user).** CORRECTS the misleading "icons via the atlas" phrasing below (lines describing reuse as baked-atlas only). The minimap calls the SAME `draw_marker()` with the SAME `IconSet(atlas, config::nativeItemIcons)` as the worldmap (map_renderer.cpp:1299,1340) → the FULL resolve chain: native map-point symbol (`category_gpu_iconId`/`category_gpu_icon_name`→`native_map_point_icon*`) → native item/loot icon (`native_item_icon`) → harvested grace sprite (`set_grace_sprite`, pumped in `draw_minimap_hud` overlay.cpp:1258) → baked atlas → circle fallback. NO render code is minimap-specific or worldmap-only. Caveat (not a bug): native engine sheets/rects are harvested LAZILY (only while the map/inventory menu is drawn) into PERSISTENT session caches (`g_map_icon_rects`/`g_map_icon_named`/`copy_sheet_cached`/`harvested_icon`) — so on a fresh boot before any map-open the minimap falls back to baked atlas; once the map is opened once, GPU icons appear in the minimap for the rest of the session. **No "port GPU icons to minimap" work needed — already shared.**

**✅ FOUNDATION VERIFIED IN-GAME (2026-06-22 #5): "works perfectly" per user** — overworld north-up corner HUD tracks the player live, map-CLOSED, all pages. RESOLVES the ⚠️ the feature hinged on: player world-pos now reliable (commit a2e3c27, `[[WCM+0x1E508]+0x6C0]`=WorldMapPointParam posX/posZ frame — SUPERSEDES the reverted physics-Vec ccb2227 note below, now stale). Remaining for FULL minimap = heading-up (player-rotation RE) + underground player-pos RE; overworld foundation is shippable.

A small always-on minimap HUD (screen corner) showing nearby goblin markers relative to the player — not the full pause-screen world map, a live mini view during gameplay.

**Building block status:** a player-centred minimap needs a reliable LIVE player world position. The physics-Vec attempt (commit ccb2227, WCM+0x1e508 chain) was REVERTED — it read overworld `local=(0,0)` and broke underground (the deep sub-offset is wrong on this build). So the player pos is NOT yet reliable. The native map CURSOR pos IS reliable (LiveView.cursorX/Z, all pages — now drives distance-adaptive, commit 65580d9) but the cursor ≠ player (it's the reticle). For a "you are here" minimap centred on the PLAYER, the underground-player-pos RE still needs finishing (RE §5 decision tree, [[overlay-pending-re]] #1). A cursor-centred mini view is possible NOW if that UX is acceptable.

**Reuse (don't rebuild):** the overlay infra in `src/worldmap/` already has everything but the camera:
- marker data = `g_buckets` / MarkerLayer (categories, collected graying, cluster keys)
- `draw_marker` + the FULL `IconSet` resolve chain (map_renderer.cpp) — native GPU map-point/item/grace icons AND baked atlas, NOT atlas-only (see verified note up top)
- ImGui DX12 overlay host (goblin_overlay.cpp, hk_present) — draw the minimap on the foreground draw list like the worldmap markers
- live event-flag reads for graying/gating

**What's NEW / needs work:**
- A minimap projection: player-centred, fixed world→screen scale (zoom slider), clipped to a circular/square HUD region in a screen corner. Simpler than the worldmap's GFx-synced projection — just `(marker.world - player.world) * scale + hud_centre`.
- **Player HEADING / facing** for rotation (north-up vs heading-up) = ADDITIONAL RE (player rotation quaternion/angle off the same physics node, near the +0x68 Vec). North-up needs no heading.
- Cull to a radius around the player; reuse distance for fade.
- Config toggles (show minimap, size, zoom, north-up vs heading-up, opacity, corner position) + F1 controls.
- Underground/DLC: same get_player_map_pos page handling; markers already grouped by page.

**Open blockers before starting:**
1. ⚠️ `[PLAYER-MAPPOS]` 2026-06-21 18:06 showed overworld `local=(0.0,0.0)` at tile (43,37) with `src=phys+0x1e508` — the Candidate-A Vec read 0,0 on OVERWORLD (implausible = likely wrong sub-offset or zeroed node). MUST get a clean live player world-pos (non-zero, tracks movement, both overworld + underground) before a minimap is meaningful. See [[overlay-pending-re]] #1 — may need to revert overworld to the CSWorldGeomMan read or fix the phys sub-offset.
2. Player heading RE (for heading-up mode) not done.

## FOUNDATION SHIPPED (2026-06-21 #3, commit 52288bf, built+deployed md5 96ba…, NOT in-game verified)
First real minimap built — OVERWORLD-only, north-up, opt-in. `goblin::worldmap::draw_minimap()` (map_renderer.cpp): reads live `get_player_map_pos` (area 60→group0 / 61→group2; underground/unknown → no-op), draws a top-right corner disc, projects `screen = ctr + (worldX-pwx)*scale, ctr - (worldZ-pwz)*scale` (north-up, V-flip matches worldmap), radius-culls, reuses draw_marker + the discover/quest/secondary/fog hide-gates, draws a yellow player dot + "N" tick. Drawn from hk_present via new `draw_minimap_hud()` + hoisted `overlay_layers()` (goblin_overlay.cpp); frame now builds when `show_minimap` even map+menu CLOSED. Config: `show_minimap`(false) + `minimap_zoom`(0.08 px/world) + `minimap_size`(130px radius) + `minimap_opacity`(0.85); F1 "Minimap (in-game HUD)" header w/ toggle + 3 sliders; persist on Save.
## PLAYER OVERLAY VERIFIED IN-GAME ✓ (2026-06-21 #3): magenta dot sits EXACTLY on the vanilla yellow pin, overworld + DLC + underground all correct. Player-pos DONE.
## MINIMAP + DISTANCE-ADAPTIVE VERIFIED IN-GAME ✓ ALL PAGES (2026-06-21 #3, commit c176b0f) — user confirmed both work overworld + underground + DLC. Minimap feature = DONE (north-up; heading-up + corner-config are optional polish). Player-pos saga fully CLOSED.
## (how it was un-gated) commit c176b0f, md5 5d1a81
Both were overworld-only due to the old bad player-pos; now un-gated. `get_player_map_pos` gained `out_group` (player's marker group via marker_group_from — projection hides the raw area). Minimap (draw_minimap) picks the player's group → draws OW/UG/DLC; fog gate uses per-group areaIdx+eyeball. Distance-adaptive: dropped `!(open_grp&1)`; euclid ramp runs where player+grace share a projected metric frame (60/61 incl. base-UG→60); DLC-UG keeps flat base_thr; param `overworld_page`→`dist_eligible`. **VERIFY: minimap shows correct nearby markers + tracks on every page; distance-adaptive ramps near→far underground too.**

## PLAYER-POS FULLY RESOLVED incl. FRAME (2026-06-21 #3, commit a2e3c27, md5 030c598) — VERIFIED in-game ✓
Frame SOLVED: `LocalPlayer+0x6C0/+0x6C8` == WorldMapPointParam posX/posZ on every page (RE validated on a grace). get_player_map_pos now feeds it as posX/posZ + bridges+projects like a marker → correct overworld + underground, map-open + map-closed. Old bug = was reading geomMgr+0x70 (physics-block frame). Full detail in [[overlay-pending-re]] #1. **VERIFY: minimap overworld → markers align + track movement.** Minimap still overworld-gated in draw_minimap; underground now valid → can un-gate as a follow-on.

## (history) PLAYER-POS chain wired (commit 29c0dde, md5 31e8c2)
RE agent delivered the runtime-confirmed chain: `[[WorldChrMan+0x1E508]+0x6C0/4/8]` = player X/Y/Z, correct map-CLOSED + underground. Wired into get_player_map_pos (position from ChrIns, area/tile from MapId). Full detail + the OPEN frame question (§4) + `[PLR2]` log in [[overlay-pending-re]] #1. **VERIFY: minimap overworld → markers should now align + track movement; share [PLR2] log to lock the world-vs-mapspace frame.** This should fix the "objects not here" bug.

## IN-GAME RESULT (2026-06-21 #3): minimap RENDERS map-closed ✓ — player-pos WAS WRONG (now wired, pending verify)
User tested overworld: **the minimap DOES draw during gameplay (map closed)** — so the foundation (frame build + foreground draw + get_player_map_pos returning non-false map-closed) WORKS. BUT it shows markers from the WRONG place = `get_player_map_pos` returns a wrong position map-closed → centres elsewhere. Confirmed = RE blocker #1 (player-pos), [[overlay-pending-re]] #1. **No minimap-code bug** (projection/orientation fine; pos is the sole fault). User is RE-ing the correct player-pos with the Windows RE agent + Cheat Engine; already has a "Pos Joueur (Heap)" address. **HANDOFF: waiting for the RE agent's findings doc** (user will paste it). NEEDED to wire it: a STATIC base (AOB or module+RVA) + pointer-offset CHAIN to the Vec (a raw heap address won't survive restart — needs a CE pointer-scan result), the Vec layout (X/Y-height/Z offsets — memory says X@+0x70/Y@+0x74/Z@+0x78 on a manager but the phys chain read 0,0; reconfirm on the new address), validated map-CLOSED + overworld(+underground). THEN: add AOB to [[re-signatures-registry]], rewrite get_player_map_pos resolution (crash-safe rpm<T>/RPM, not __try — [[clang-cl-seh-noinline]]), keep the fallback. Distance-adaptive + minimap both unlock.

**⚠️ THIS WAS THE map-CLOSED player-pos TEST → PASSED for rendering, player-pos still wrong.** If the minimap markers move correctly as the player walks (map closed, overworld) → `get_player_map_pos` works as a live gameplay source = minimap viable. If markers DON'T track / player drifts → the map-closed overworld pos is also unreliable (would need RE). **VERIFY in-game: enable Show minimap in F1, walk around the overworld → markers slide past, stay geographically correct; tune zoom/radius. Underground → minimap vanishes (expected).**
**STILL NEEDED after verify:** player HEADING RE (heading-up mode); underground player-pos RE (overlay-pending-re #1) for UG minimap; optional: corner-position config, circular clip (currently rect clip + radius cull), fade-by-distance.

**Status: FOUNDATION SHIPPED (was BACKLOG).** Prereq player-pos: overworld via get_player_map_pos — THIS BUILD tests whether it holds map-closed. The worldmap overlay ([[overlay-rendered-markers]]) is the active project; minimap is now in-flight alongside it.
