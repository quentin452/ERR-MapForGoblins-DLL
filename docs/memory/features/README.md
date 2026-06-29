# Features

Fork functionality and the reverse-engineering that became a shipped feature. Each topic below
reconciles the relocated raw notes (originally split across the Linux and Windows agent memories)
into a single current-truth entry. Status reflects the **current code**, not the note's original wording.

## Map overlay & UI
- **Self-rendered overlay markers** [shipped] — ImGui/DX12 overlay is the sole map path; eliminated
  open-freeze, live-refresh and phantom-marker problems at once → [overlay-rendered-markers](overlay-rendered-markers.md)
- **In-game settings overlay** [shipped] — DXGI-Present hook, F1 toggle, live toggles, save-to-INI → [thread6-overlay](thread6-overlay.md)
- **Per-section icon toggle** [shipped] — flips injected rows' `areaNo` 99/restore, no param rebuild → [thread3-grace-toggle](thread3-grace-toggle.md)
- **Item/object search bar** [shipped] — name search + locate/pan inside the hooked map step → [overlay-item-search-bar](overlay-item-search-bar.md)
- **Minimap HUD** [shipped] — player-centred corner minimap, reuses the marker/atlas chain → [minimap-future-feature](minimap-future-feature.md)
- **Unicode font** [shipped] — embedded DejaVu Sans TTF over ImGui's Latin-1 default → [imgui-unicode-font](imgui-unicode-font.md)

## World-map RE (live projection / position / icons / fog)
- **Live world→map projection** [shipped] — `liveProjection=true`; baked LegacyConv kept as map-closed fallback → [worldmap-projection-re-solved](worldmap-projection-re-solved.md)
- **Player position** [shipped] — `[[exe+0x3D65F88]+0x1E508]+0x6C0/6C4/6C8`, == WorldMapPointParam posX/posZ → [overlay-pending-re](overlay-pending-re.md)
- **Native map-point icons** [shipped, Boss+Grace ceiling] — Oodle IAT hook harvests DDS; only grace/boss pins ever resident → [native-icon-render-pipeline](native-icon-render-pipeline.md)
- **Fog-of-war reveal oracle** [solved, last-mile open] — `FUN_140886560(VM,layer,tileId)`; revealed iff `(flags&MASK)==0` → [worldmap-unsearched-fog-mask](worldmap-unsearched-fog-mask.md)

## Loot & data (no-bake)
> Cross-machine cluster — the loot system spans many notes; the as-built source of truth is here.
- **Loot from real files** [shipped] — runtime treasure + AEG + enemy-drop + EMEVD passes; bake = oracle → [handoff-loot-from-real-files](handoff-loot-from-real-files.md)
- **AEG collectible source** [shipped] — `loot_collectibles`, AEG099 asset → `pickUpItemLotParamId(+0xb8)` → [aeg-collectible-source](aeg-collectible-source.md)
- **Loot identity / ERR-additive** [shipped, off-bake complete] — runtime lot-link code DORMANT (`diagLotMemscan` off) → [loot-identity-stable-err-additive](loot-identity-stable-err-additive.md)
- **Sealed-chest registry RE** [closed] — sealed-chest loot is NOT pre-open resident; walker exists for spawned loot → [fieldins-pool-registry-re](fieldins-pool-registry-re.md)
- **World-feature MSB identity table** [active] — off-bakes ~1300 static markers (`WORLD_FEATURE_MODELS`, now 9 rows) → [world-feature-msb-identities](world-feature-msb-identities.md)
- **Marker coverage & quest-NPC layer** [shipped] — projection fixes, Royal-Capital eviction, 344-marker NPC layer → [mapforgoblins-map-freeze](mapforgoblins-map-freeze.md)

## Item classification
> Two notes describe the same classifier — `phase3` is the as-built, `er-item-taxonomy` the reference.
- **Live taxonomy classifier** [shipped, sole] — `category_from_taxonomy(goodsType@+0x3e, sortGroupId@+0x72)`; 0-drift, replaced `ITEM_ICONS` → [phase3-taxonomy-map-validated](phase3-taxonomy-map-validated.md) · [er-item-taxonomy-sortgroupid](er-item-taxonomy-sortgroupid.md)
- **`ITEM_ICONS` deleted** [done; caveat] — table + dead per-item path removed; **stale copies still linger in `src/generated_erte`, `generated_vanilla`, `generated_convergence`** → [item-icons-table-deleted](item-icons-table-deleted.md)

## Quests
> Quest Browser is the supported path; the older quest-aware NPC gating is legacy-in-practice.
- **Quest Browser** [shipped] — ordered steps, persisted checkmarks, missable warnings, EMEVD grey-out → [quest-browser](quest-browser.md)
- **Quest-aware NPC markers** [shipped, legacy] — QuestGate table (55 gates/35 NPCs) parks non-active NPCs → [questlog-flag-data-reference](questlog-flag-data-reference.md)

## Other shipped
- **Coverage-gap detector** [shipped, opt-in] — SetEventFlag + AddItemFunc correlation toast → [thread7-coverage-detector](thread7-coverage-detector.md)
- **ERR underground grace gate** [shipped] — `iconId==44`; graces read live from BonfireWarpParam → [err-underground-grace-gate](err-underground-grace-gate.md)
- **Grace pin suppression + map-point rect harvest** [shipped] — `grace_suppress_native` (default off) → [session-2026-06-23-map-icons](session-2026-06-23-map-icons.md)

## Ideas / not-yet-built
- **Real inventory-icon atlas** [open, unmerged branch] — draw 00_Solo icons on category markers → [category-icons-00solo-atlas](category-icons-00solo-atlas.md)
- **Entity/asset radar** [idea] — reuses CSWorldGeomMan walk, no new RE for chest radar → [entity-radar-foundation](entity-radar-foundation.md)
- **Active input-device flag** [open RE] — single source for hint-switch / cursor-recenter / map-drift → [input-device-active-flag](input-device-active-flag.md)
- **Prologue-flag map gate** [idea] — event flag 120 gates the map off in Chapel of Anticipation → [er-prologue-flag](er-prologue-flag.md)
- **Bestiary / boss kill-count** [idea] — tier-1 defeated-flag checklist needs zero new RE → [bestiary-killcount-idea](bestiary-killcount-idea.md)
- **Minimap via ER native compass** [idea, RE needed] — reuse ER's own compass instead of our minimap
  draw, to cut clutter; needs RE of the native compass widget. Cross-ref [minimap-future-feature](minimap-future-feature.md)
- **Category coverage — 31 MapGenie-missing categories + "small enemy" category** [idea/backlog] — ~31
  MapGenie categories are still unwired; a dedicated *small/Commoner enemy* category is hypothesised to
  avoid icon explosion — supported by MapGenie itself NOT listing Shadow Commoners. **Keep the still-useful
  parts of the `data/` folder** (mostly stale post-no-bake) precisely to feed these future categories.
  Cross-ref [world-feature-msb-identities](world-feature-msb-identities.md), [../process/map-data-obs-moore-enemies](../process/map-data-obs-moore-enemies.md)
