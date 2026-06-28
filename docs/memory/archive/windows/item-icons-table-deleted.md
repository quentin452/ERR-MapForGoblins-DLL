---
name: item-icons-table-deleted
description: "ITEM_ICONS table DELETED (branch feat/drop-item-icons-table) — it held map-point FRAMES not inventory iconIds, fed a no-op render path; category→icon is already live. The premise \"read iconId from param\" was wrong."
metadata: 
  node_type: memory
  type: project
---

ITEM_ICONS table + the per-item icon render path are GONE (commit f3aaa41, branch
feat/drop-item-icons-table, default/err profile builds clean, **RUNTIME-VALIDATED 2026-06-27**:
log 17:22 run = new DLL [ITEMCLASS] "catch-all tail surface" string, [COVERAGE] TOTAL baked=0
disk=8381 live=469, render.worldmap.markers ran ~45µs, zero errors/asserts/icon null-derefs).
Bonus: [EQUIPADDR] probe read Protector iconId u16@0xa6=45077 — a large inventory-atlas id,
confirming the param iconId ≠ the 376..414 map-point frames the table held.

**The non-obvious finding that redirected the task:** the original ask was "icons live →
item_icon_id_live reads iconId direct from the param (Goods@0x30, Weapon@0xBE, Protector@0xA6,
Accessory@0x26, Gem@0x04)". That premise was WRONG:
- ITEM_ICONS stored worldmap MAP-POINT FRAME ids (our custom 349..440 range, e.g. weapons→380,
  armour→381, greases→408), keyed by CATEGORY — NOT the inventory iconId (a different number
  space, thousands). The param's iconId@0x30 etc. is the inventory atlas, unrelated.
- That category→frame mapping is ALREADY live via item_marker_category() + category_icon_key()
  / category_gpu_iconId(). So the table was a redundant bake.
- It fed a NO-OP path: Marker::icon_id → ItemIconProvider → overlay::native_item_icon() keyed
  harvested_icon on the MENU_ItemIcon_<id> inventory sheet, but got map-point frames → "missed by
  construction", always fell through to the atlas. [[runtime-icon-coverage]]: per-item inventory
  harvest is a PROVEN runtime dead-end (no global page; force-load=atlas-no-rects).
- The 3 profile consts in the file (ANON/CLUSTER/CLUSTER_DONE icon ids) were DEAD — zero read
  sites; the overlay draws clusters/anon with ImGui primitives, not game frames.

<user> chose "Delete table + dead path" (not revive real per-item icons). Removed: the 2 generated
files (+ stale per-profile copies), item_icon_id()/lookup_item_icon(), Marker::icon_id,
ItemIconProvider/IconKey::ItemIcon, overlay::native_item_icon(), generate_item_icons_cpp + CMake/
pipeline wiring. KEPT (still used by the category map-point path + debug panels): the MENU_ItemIcon
harvest machinery, harvested_icon, ensure_item_icon_srv, the native_item_icons config (now gates the
MENU_MAP_<NN> symbol path only), and data/item_icon_table.json (the category-exceptions generator
still reads it). See [[delete-generate-data-path]] (same generate_data shrink effort).

LESSON: trace what a "baked" value actually IS before replacing it with a live read — the field
name ("inventory iconId") lied; the data was category map-point frames.
