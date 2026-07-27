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

**Follow-up (2026-07-23): tier-3 is a stopgap, not the real answer.** Bosses now show WITH names on vanilla
(fixed the 91/1183 bug — `live_name` was only set on the first-seeded instance per type; now set on every
instance of a synthetic-textId1 type). BUT tier-3 is a NAME-SOURCE tier, not an "is-boss" signal, so it
mis-identifies / duplicates (observed: multiple "Mimic Tear", clone-model bosses collapse). The item-search
name↔position list can't be reused — it's POI-only (`WorldMapPointParam`), not entities.

**RE SOLVED (2026-07-27) — implementation still TODO.** The proper mod-agnostic source is the EMEVD
instruction **`2003[011] HandleBossHealthBar(state, entityId, slot, nameId)`**, joined with the MSB enemy
placement (entity → position) and `GameAreaParam` (rowId == boss entityId → `defeatBossFlagId`): **254
bosses with name + position on a vanilla+randomizer install**, per-ENCOUNTER, name as a plain FMG id.
Full method, counts, live validation and wiring notes: `docs/re/cross_mod_boss_naming_re_findings.md`
(repro: `tools/_probe_boss_enum.py`). Two dead leads killed there: `NpcParam` has **no** GameAreaParam id
(no `gameArea*` field in any of the 194 paramdefs), and `GameAreaParam` alone carries neither name nor
position.

**IMPLEMENTED 2026-07-27 (tier 4).** `msbe::parse_emevd_boss_bars` extracts `2003[11]`;
`worldmap::emevd_boss_bars()` caches `entityId → nameId` (its own EMEVD walk, deliberately NOT gated on
any loot toggle — the quest-pin gating bug two blocks up in `map_entry_layer` is the same trap);
`enemy_display_name(npcParam, model, &tier, entityId)` gains **tier 4**, ranked above everything.
`build_live_bosses` passes the entity, and **only tier 4 may SEED a boss type** (see below).
Cross-thread note: the marker build runs on the disk WORKER while the enemy-tag reconciler runs on the
PRESENT thread, so the `npcParam → nameId` registry (`register_boss_bar_name`, how the tag reaches tier 4
without an entity id) is mutex-guarded; the present thread must never call `emevd_boss_bars()` itself
(that first call walks ~589 emevds).

**Tier 3 must never decide WHO is a boss — measured live 2026-07-27.** With tier 3 allowed to seed, the
map drew **1197** boss markers over 241 types on vanilla+randomizer, against **251** boss-bar entities on
that install (~5x). The shape: 194 types had exactly 1 marker, while **20 types produced 891 markers**
(worst: 269, then 120, 91, 61…). Cause: tier 3 scans 1000 suffixes of `900000000 + model*1000` and calls
any hit a field boss — a MODEL property. `vmap ename 12 1 0` on one underground tile: 327 enemies → 224
tier-0 (correct), **98 tier-3 "bosses" over 12 models (c3320 alone 52 placements)**, 4 tier-4, 1 tier-1.
Since a type is then stamped on every tile holding that model, a common model explodes into hundreds of
markers. Fix: `if (tier != 4) continue;` — tier 3 survives only as a NAME fallback. ERR's WMP pins keep
their own seeding path (name match), unaffected.

**Tier-3 is WRONG for NAMES too, not merely imprecise — proven live 2026-07-27.** At Gatefront (area60 grid 42,36) the
mod named the boss `Tree Sentinel` (tier 3) while the game's own health bar read **`Black Tree Kindred
Sentinel`**. Cause, read live: `NpcParam[47701242].nameId == 0` → the resolver falls through to the
`900000000 + model*1000` band, which names the **vanilla chr model** (c3251), so any mod that reskins or
reassigns a boss gets the wrong name by construction. **Same root cause hits a SECOND consumer**: the
world-space 3D enemy tag (`config::nameEnemyBosses`) shows that same wrong name in-world. Fix the shared
resolver by sourcing the name on **entityId** and both are repaired at once.
