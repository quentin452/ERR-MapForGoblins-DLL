# Windows RE brief — the OPEN-MAP region/page id (for overlay region gating)

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.** Goal: find the field
that identifies the **currently-open world-map region** — overworld (Lands Between),
underground, DLC overworld (Land of Shadow), DLC underground — so the overlay draws only
that region's graces (today everything piles into one space).

## Why the obvious signals failed (live-confirmed)

- **`DAT_143d6cfc3` (the "sublayer" flag) is DEAD** — reads `0` on every map (overworld,
  underground, DLC). Not the discriminator.
- **`fullRect` (WorldMapArea +0x350) is CONSTANT `[0,0,10496,10496]`** on all maps — all
  maps share one WorldMapArea canvas. Not it.
- **Player position ≠ open map** — the player can open ANY discovered map from anywhere, so
  `get_player_map_pos` / "player in DLC" does not tell which map is open.

So the open-region must be read from the **world-map UI state**.

## What we already have (use it)

- The **`CS::WorldMapDialog`** object is resolved at runtime: `dialog = cursor − 0x2DB0`,
  where `cursor` is the `CS::WorldMapCursorControl` (vtable RVA `0x2b29a90`), reached via the
  CSMenuMan → dialog bounded walk (`goblin_worldmap_probe.cpp`). So both `dialog` and the
  WorldMapArea `view = *(cursor+0xF0)` are in hand.
- WorldMapDialog ctor `FUN_1409cf8f0` (sizeof `0x3ed0`), base `WorldMapDialogBase`
  `FUN_1409c1080` (the one that builds the cursor at `+0x2DB0`).

## Live delta-scan candidates (fields that FLIP when switching maps)

A delta-scan of small int32s on `dialog`/`view` while the user switched maps flagged:

| field | values seen | note |
|---|---|---|
| `view + 0x370` | `0` ↔ `10` | paired with +0x374; CT notes called +0x370 a "unified-view areaNo" |
| `view + 0x374` | `1` ↔ `11` | moves with +0x370 |
| `dialog + 0x98` | `1` ↔ `2` | clean 2-value — candidate "which map / tab" |
| `dialog + 0x14` | `0` ↔ `3` | moves with +0x5c |
| `dialog + 0x5c` | `0` ↔ `3` | |

(The user couldn't map values→regions by eye — several fields move together. RE to pick the
authoritative one and decode it.)

## The task

1. Identify the field that holds the **open-map region/page id** (overworld / underground /
   DLC-OW / DLC-UG), and how its value decodes to a region. Statically: find where the
   world-map open/tab-switch path WRITES it (the menu-open / sublayer-toggle / DLC-map-open
   handlers), and where the marker-display/gating code READS it to decide which markers show.
   The game itself gates markers per open page — follow THAT read (it compares a marker's
   area/tab to the open page).
2. Resolve the **value → region mapping**. The mod classifies graces into:
   base-overworld (areaNo 60/61), base-underground (areaNo 12), DLC (tab id 6800-6999 /
   areaNo 40-43). Give the open-region field's value for each of those 4 maps.
3. If the open map is identified by a **tab id** (the mod already uses tabIds: underground
   12000/12001/12002, DLC 6800..), give the field that holds the **open tab id** — that's the
   finest, most direct key (matches the mod's `GRACE_ANCHORS.tab_id`).

## CRITICAL — the field must be VIEW-driven, not player-driven

Verify the candidate field reflects **which map the user is LOOKING AT** (the open map
page the user toggled to), NOT where the **player physically is**. The overlay needs the
*viewed* region: the player can stand in Limgrave and open the DLC or underground map, and
the overlay must gate to the OPEN map, not Limgrave. So check both axes independently:
- switch the map view (overworld → underground → DLC) **without moving the player** → the
  field MUST change;
- **move the player** into a different area while NOT changing the open map (or with the map
  closed) → the field must NOT change for our purpose.

If the only field you find is player-driven (tracks physical area, not the viewed map),
say so explicitly — then the open-map id lives elsewhere (the menu/tab-selection state),
and that's the one we need. A field that changes on BOTH is ambiguous — flag it and prefer
a purely view/tab-selection-driven one.

## Deliverable

- The field (offset on `dialog` or `view`, or a static/singleton) for the open region/page/
  tab, with its read C++ snippet and the value→region table for the 4 maps.
- Confirm against the 4 map types live (overworld / underground / DLC-OW / DLC-UG).
- Note version-stability (offset on the dialog struct → document how to re-find it).

## Handles

- cursor vtable RVA `0x2b29a90`; `dialog = cursor − 0x2DB0`; `view = *(cursor+0xF0)`
  (WorldMapArea, pan +0x378, zoom +0x380, fullRect +0x350=10496).
- WorldMapDialog ctor `FUN_1409cf8f0`; base ctor `FUN_1409c1080`.
- delta-scan candidates above (view+0x370/0x374, dialog+0x98/0x14/0x5c).
- Marker display/gating: build `FUN_140a82a80` / reconcile `FUN_140a832a0` over
  `CSWorldMapPointMan+0x398`; the per-row show predicate `FUN_140d58470` (reads the row's
  display-group flag) — somewhere here the open-page is compared to the marker's area/tab.
