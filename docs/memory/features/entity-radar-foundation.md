---
name: entity-radar-foundation
description: "Future real-time entity/asset radar — loaded-only RE already done is exactly right; asset radar = zero new RE, enemy radar needs only the abandoned WorldChrMan ChrIns walk"
metadata: 
  node_type: memory
  type: project
---

Forward-looking foundation note (commit 09121dc, `docs/re/entity_radar_foundation.md`). The loaded-only
runtime RE that was USELESS for the whole-map overlay (streaming limit, no global registry) is **exactly
right for a real-time radar** around the player — a radar only ever shows the loaded bubble, so the limit
is a feature, not a bug.

**Why:** if a radar is ever wanted, most of it is already RE'd + running — don't re-scope it as big new work.

**How to apply — an ASSET/CHEST radar needs ZERO new RE**, reuse what's live:
- `CSWorldGeomMan` walk → loaded instances (`src/goblin_collected.cpp`, WORLD_GEOM_MAN_SLOT) = enumerate nearby assets.
- per-instance runtime pos (`MsbPart+0x20` or `CSWorldGeomStaticIns+0x250` vec4) + partName = blip pos + label.
- `FieldIns→lotId@+0x50` → `resolve_loot_item_textid` = "this chest contains X" (loaded-only, see [[loot-identity-stable-err-additive]]).
- block→world / map-UI transforms (FUN_1408775e0 / FUN_140876140) = place blips. Player-pos chain = radar centre.

**An ENEMY radar's ONLY missing piece** = `WorldChrMan` ChrIns enumeration — the enemy ChrIns list off WCM
+ per-ChrIns pos (analog of `[[[[WCM+0x1E508]+0x58]+0x10]+0x190]+0x68]+0x70/78`). That walk was ABANDONED in
`windows_enemy_boss_runtime_pos_re_findings.md` (useless for boss-drift/map) but is precisely what an enemy
radar needs. Asset radar needs none of it.

**Scope reminder:** radar = live bubble (this RE); world map = whole map (baked, mandatory). Different
features, different data sources — never mix. Loaded-only proven safe ([[live-param-vs-baked-data]],
global-registry REFUTED e1b502b).
