# Entity-radar foundation (reuse of the loaded-instance RE)

Forward-looking note. The runtime RE done for the loot/boss/global-position investigations is
**loaded-only** — useless for the whole-map overlay, but **exactly right for a real-time entity/asset
radar** around the player. A radar only ever shows the loaded bubble, so the streaming limit is a
feature here, not a bug. If we ever build a radar, this is the foundation — most of it is already
RE'd and running.

## What a radar reuses (already live)

| Piece | Where | Radar use |
|---|---|---|
| `CSWorldGeomMan` walk → loaded instances | `src/goblin_collected.cpp` (`WORLD_GEOM_MAN_SLOT`) | enumerate nearby assets/chests |
| Per-instance runtime pos (`MsbPart+0x20`, or `CSWorldGeomStaticIns+0x250` vec4) + `partName` | [windows_live_loot_position_re_findings.md](windows_live_loot_position_re_findings.md) | blip position + label |
| `FieldIns → lotId @+0x50` → `resolve_loot_item_textid` | [windows_runtime_asset_to_itemlot_re_findings.md](windows_runtime_asset_to_itemlot_re_findings.md) | "this chest contains X" |
| Block→world / map-UI transforms (`FUN_1408775e0` / `FUN_140876140`) | `re_findings_playerpos.md` | place blips in screen/world space |
| Player pos chain | `re_findings_playerpos.md` | radar centre |

## The one missing link (for an ENEMY radar)

Enemies/NPCs are **`ChrIns` off `WorldChrMan`**, not geom instances. The `WorldChrMan` enemy
enumeration was **abandoned** in
[windows_enemy_boss_runtime_pos_re_findings.md](windows_enemy_boss_runtime_pos_re_findings.md)
(useless for the boss-drift / map case) — but it is **exactly** the piece a live enemy radar needs.
That RE (enemy ChrIns list off WCM + per-ChrIns pos `[[[[WCM+0x1E508]+0x58]+0x10]+0x190]+0x68]+0x70/78`
analog) is the only new work for enemies. Asset/chest radar needs no new RE.

## Why this is settled & safe to lean on

Loaded-only was **proven** (no global registry):
[windows_global_item_position_structure_re_findings.md](windows_global_item_position_structure_re_findings.md)
(fresh-session scan 22/22 far tiles absent; RTTI `CS::CSWorldGeomStaticIns`; per-region SIMD writer).
So a radar reading these resident instances is reading the engine's own live truth for the bubble —
the right tool for the right job.

**Scope reminder:** radar = live bubble (this RE). World map = whole map (baked, mandatory). Different
features, different data sources — don't mix them.
