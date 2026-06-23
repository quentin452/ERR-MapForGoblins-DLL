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

---

## RESULTS — open-page field PINNED, value→region is runtime (2026-06-20, Ghidra headless)

Scripts `find_region.java` / `find_region2.java` (`D:\ghidra_scripts`, outputs `out_region*.txt`),
app 2.6.2.0.

### The authoritative field: `dialog + 0xA88` (int32) = open menu page id

The dialog's `[0x151]` slot (`undefined8*` units ⟹ byte `0xA88`) **is the selected map page**:
- **Written** by the dialog setup `FUN_1409be5e0` @ `0x9be5e0` L259 (`*(int*)(dialog+0xA88) = page`)
  from the open parameter, and on page-change via `FUN_1409c8120(dialog, *(int*)(dialog+0xA88))` (L984).
- **Decoded** by the page→mapno getter `FUN_1409c4900(page, flag)` @ `0x9c4900`: returns the page
  ≈ map-no as-is (`page==1` → `_DefaultMapNo` resolution; negatives clamp to 0). So the field *is*
  effectively the open map-no / page index.
- **View-driven by construction** — it's the menu's selected page, written by the open/tab path,
  independent of player physical area. Exactly the axis the brief requires.

### Secondary identity: `view + 0x370` / `+0x374` = WorldMapArea ctor params

`WorldMapArea` ctor `FUN_1409cb9c0` @ `0x9cb9c0` writes `*(int*)(view+0x370)=param_5` and
`*(int*)(view+0x374)=param_6` (areaNo-like identity baked at construction). Matches the delta-scan
`0↔10` / `1↔11`. Only one area is constructed per dialog (in setup, at `dialog+0x4fb`), and the
decompiler truncated the ctor call args, so **the literal region constants are not statically
liftable** — they come out at runtime.

### Dead ends reconfirmed statically

- `FUN_140d58470` (the brief's "show predicate") is the **grace event-flag check**
  (`_Verify{Enable,Disable}EventFlag` = `FUN_140d58640`/`FUN_140d58550`), NOT a region gate.
- `FUN_1409c6f70` (called by the base ctor with the page id) is an **overlay-layer changeability**
  helper (`_IsChangeableOverlayLayer`, reads the overlay toggle byte `DAT_143d6cfc0`), NOT the region.

### Deliverable for runtime (quentin)

Read these three on each of the 4 maps and pick the one giving 4 distinct values (or a clean
`(page, sublayer)` pair — underground may need a combo since the standalone sublayer byte is dead):

| field | type | source |
|---|---|---|
| **`dialog + 0xA88`** | int32 | menu page id (engine's own decode key) — try FIRST |
| `view + 0x370` | int32 | WorldMapArea areaNo-like ctor param (delta-scan `0↔10`) |
| `dialog + 0x98` | int32 | delta-scan `1↔2` (clean 2-value; static role unconfirmed) |

`dialog = cursor − 0x2DB0` (already resolved O(1) via the CSMenuMan walk in
`goblin_worldmap_probe.cpp`); `view = *(cursor + 0xF0)`. Version-stability: re-find `0xA88` as the
int written by `FUN_1409be5e0`/read by `FUN_1409c4900`; re-find `view+0x370/0x374` as the
`WorldMapArea` ctor (`FUN_1409cb9c0`) params 5/6.

---

## ✅ SOLVED — full 3-region gate, LIVE-CONFIRMED (2026-06-20)

The page (`0xA88`) only splits **base vs DLC** — Overworld and Underground share it (page=0), because
**underground is applied internally as `page + 1`**, not stored in the page field. The live OW↔UG state
lives in a **separate readable byte behind a deref**, found via the 3 map-switch buttons
(`CS::WorldMapSwitchDialog` / `CS::WorldMapViewModel`).

### The two O(1) fields

From the O(1)-resolved cursor (`dialog = cursor − 0x2DB0`):

```c
int     page  = *(int*)(dialog + 0xA88);                            // 0 = base, 10 = DLC
uint8_t layer = *(uint8_t*)( *(void**)(dialog + 0x2B68) + 0xB8 );   // 0 = surface, 1 = underground
```

- **`layer`** = the surface/underground state. **Setter `FUN_1409c40f0`** writes
  `*(*(dialog+0x2B68)+0xB8) = target_layer`, then applies `FUN_1409c8120(dialog, page + (layer?1:0))`
  (→ underground = page+1 internally). Confirmed by the button-state fns: **`FUN_1409d5ce0`** (layer==0 →
  shows the "Afficher souterrain" button), **`FUN_1409d5e40`** (layer!=0 → shows "Afficher surface").
- `FUN_1409c6f70` (`_IsChangeableOverlayLayer`, takes `page@0xA88`) only says whether the toggle is
  *available* — not the current state. The global `DAT_143d6cfc3` is transient/render-only (set per-frame
  in projection `FUN_1409ce190/cd790`), `DAT_143d6cfc0` is dead — **ignore both**; use the deref byte.

### The gate

```c
if      (page == 10) region = DLC;          // areaNo 40-43
else if (layer == 0) region = OVERWORLD;    // areaNo 60/61
else                 region = UNDERGROUND;  // areaNo 12
```

### Live verification (CT `MapForGoblins_region.CT` entry 30 = `[[dialog+0x2B68]+0xB8]`)

| map | `page (0xA88)` | `layer (deref)` | → region |
|---|---|---|---|
| Overworld | 0 | **0** | OVERWORLD ✓ |
| Underground | 0 | **1** | UNDERGROUND ✓ |
| DLC | 10 | 0 | DLC ✓ |

Two reads, zero scan. All other candidates (`view+0x370/0x374`, `dialog+0x98/0x14/0x5c`, `cfc3/2/0`)
are unnecessary — `page` + `layer` are the only fields needed.

**Version-stability:** re-find `layer` via the setter `FUN_1409c40f0` (writes
`*(*(dialog+0x2B68)+0xB8)` and calls `FUN_1409c8120(dialog, page+layer)`), or the button-state fns
`FUN_1409d5ce0`/`FUN_1409d5e40`. `page` as before (`FUN_1409be5e0`/`FUN_1409c4900`).
