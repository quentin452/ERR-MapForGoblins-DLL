---
name: delete-generate-data-path
description: "Phase-2 pipeline cleanup DONE: the 3.67 MB _map_entries_full.cpp intermediate is eliminated (both consumers retired). generate_data is NOT deletable — it owns 5 live tables."
metadata: 
  node_type: memory
  type: project
---

# Phase-2 intermediate elimination — DONE (branch feat/phase2-drop-intermediate)

**★ SHIPPED 2026-06-27** (2 commits on `feat/phase2-drop-intermediate`, stacked on master merge 4a7716d;
clang-cl build links clean; **runtime in-game validation still PENDING — <user> tests**). The 3.67 MB
`data/_map_entries_full.cpp` intermediate is **eliminated** — generate_data no longer emits it, both
downstream consumers retired.

- **(B) commit 1c52bc5 — retire LOCATION_ALT + generate_location_overrides.** `LOCATION_ALT` had NO
  runtime consumer once MAP_ENTRIES went empty-stub (the textId2-overwrite path was already gone).
  `LOCATION_COMPOSE` (the 2 Hallowhorn-Grounds disambig labels) WAS still read in goblin_messages, but
  its composed ids (999M+) are only referenced by LOCATION_ALT rows → it injected orphan FMG strings
  nothing pointed at → also dead. Deleted both + the compose block + generate_location_overrides.py +
  the build stage + the 2 dead includes.
- **(A) commit d8259a3 — read the part's ACTUAL model LIVE; retire GEOF_MODEL_OVERRIDES.** THE one real
  RE. ERR substitutes some gather-node models while keeping the vanilla part NAME (part
  `AEG099_753_9000` instantiating DLC `AEG463_860`; `AEG099_720_xxxx` → `AEG099_710`). GEOF save
  entries key on the ACTUAL model hash, so name-prefix bucketing left substituted nodes ungrayed. The
  old baked fix (generate_geof_models → 84 row_id→model rows) is replaced by reading the real model
  from the MSB:
  - **★ PINNED OFFSET: MSBE Part modelIndex = u32 @ part+0x14** → indexes the MODEL section
    (secs[0]) name list (each MODEL entry's name @ entry+0x00, same eio as parts). Validated
    **11415/11415** vs SoulsFormats `Part.ModelName` on m12_02 + m60_40_52 (the offsets +0x08/0x10/0x18
    all fail; +0x14 is unique). Probe was tools/scratchpad probe_modelindex.py (ephemeral) — re-derive
    by loading any tile MSB + matching candidate int offsets against SoulsFormats oracle.
  - msbe_parser parses the MODEL names + sets `Asset.modelName`; plumbed through `DiskCollectible.modelName`
    → `RuntimeEntry.model_name` → `insert_tracked_entry`, which now keys GEOF (g_tile_slot_to_row + the
    g_tracked_model_ids filter) by the ACTUAL model while WGM alive-tracking still keys on the part NAME
    (tracks BOTH prefixes, same as the old bake). 247 substituted nodes total (bake only covered 84).
  - deleted the dead MAP_ENTRIES seed loop in goblin_collected::initialize (its sole remaining use of
    the override table) + generate_geof_models.py + goblin_geof_models.{cpp,hpp}.
- **(C) same commit d8259a3 — drop the intermediate + purge the bake-table machinery.** With both
  consumers gone, the full MapEntry table has NO reader → removed `parse_massedit_files`, `FIELD_MAP`,
  `CPP_FIELD_ORDER`, `format_value`, `load_piece_metadata`, `_load_lot_linkage`, `_LOOT_SRC_ENUM`,
  `ERR_ONLY_FILES`, the de-overlap spiral, and the full-table emission. generate_data now writes only
  the constant no-bake stub.

## ⚠️ CORRECTION to the old plan: generate_data.py is NOT deletable
The earlier note assumed generate_data only owned icons+enemy-names → deletable after splitting them
out. **WRONG.** generate_data still owns **5 live runtime tables**: `goblin_item_icons.cpp` (1818),
`goblin_category_exceptions.cpp` (133), `goblin_enemy_names.cpp`, `goblin_name_aliases_en.cpp` (3276),
`goblin_legacy_conv.hpp` (94) — plus the map_data stub. CATEGORY_MAP is shared by item-icons +
category-exceptions. So generate_data stays; it just no longer parses MASSEDIT or emits the bake table.
Regen-validated: those 5 tables stay byte-identical, only the stub comment changed.

## NEXT
- **Runtime validation (<user>):** a substituted-model gather node must gray on collect — find one via
  data/all_gathering_nodes_final.json where `model != name.rsplit('_',1)[0]` (e.g. AEG099_753 in
  m60_40_52, AEG099_720 in m12_02), collect it in-game, confirm the icon grays + stays gray on a tile
  reload/restart (the visible-after-restart bug the override fixed).
- Phase-2 pipeline cleanup is essentially complete. See [[nobake-endgame-roadmap]] (Phase 3 = the
  offset-free "industrial" refactor) and [[nobake-coverage-scoreboard]] (the 16 irreducible residual).
