---
name: mod-agnostic-boss-markers
description: "Boss markers were ERR-only (seeded from textId2==5100 WorldMapPointParam pins, absent on vanilla/randomizer); now also seeded mod-agnostically from the tier-3 field-boss NpcName band via Marker::live_name."
metadata:
  node_type: memory
  type: project
---

# Boss markers were ERR-only — now mod-agnostic (tier-3 field-boss band)

**Symptom (2026-07-23, found on a randomizer run tested on Windows).** Zero boss markers (and no Great
Runes, which derive from boss positions) on a randomizer/vanilla run; correct on ERR. Diagnosed by diffing
two session logs: the randomizer's `WorldMapPointParam` had **0** rows with `textId2==5100`; ERR had 217.

**Root cause.** `build_live_bosses()` (`src/worldmap/map_entry_layer.cpp`) seeded the set of known boss
TYPES (`marked`) ONLY from `WorldMapPointParam` rows with `textId2==5100` — an **ERR-specific** map-pin
encoding. The mod-agnostic enemy-supplement (live MSB enemy scan) then only *completes instances of types
already in `marked`*. So off-ERR: no `5100` pins → `marked` empty → supplement matches nothing → 0 bosses.
Violated the mod-agnostic prime directive (an ERR-baked source with no fallback).

**Fix.** Seed boss types mod-agnostically from the **tier-3 field-boss / miniboss NpcName band**. The name
resolver already classifies enemies by tier: `goblin::enemy_display_name(npcParam, model, &tier)` returns
`tier==3` when the name comes from the vanilla boss band (`900000000 + model*1000 + suffix`, read raw via
`raw_message_utf8` — see `goblin_enemy_names.cpp`). In `build_live_bosses`, when a scanned enemy resolves
via tier 3 and has no ERR pin, seed a NEW boss type from it (WorldBosses category icon).

**Name plumbing (the non-obvious part).** A tier-3 boss name is a runtime string in a raw NpcName slot the
marker's `lookup_text_utf8` band-router can't resolve (it explicitly bypasses the router). Markers carried
only an FMG `name_id`, so:
- Added `std::string Marker::live_name` (set AFTER the aggregate init, like `lotId`/`item_icon_id`; adding
  it mid-struct broke the positional `Marker m{...}` init — keep new fields at the END).
- Display paths prefer `live_name`: VWM tooltip (`plot()` `vname` arg), native/minimap tooltip
  (`map_renderer.cpp` name resolve), and item search (`panel_search.cpp` name cache).
- Each mod-agnostic boss TYPE gets a stable **synthetic `name_id`** (`0x60000000 + counter`, via
  `MarkedBoss.textId1` → push_marker's non-lot fallback) so the search dedup + on-map ring (both keyed on
  `name_id`) still collapse a boss's N instances into one — the synthetic id is never displayed (all paths
  use `live_name`).

**Guardrail.** Any "what enemies are bosses?" question is answered mod-agnostically by
`enemy_display_name(..., &tier)` tier==3, NOT by an ERR map-pin field. See [[overlay-input-unfocused-hooks]]
for the same session's input fixes.
